#!/usr/bin/env python3
"""Run the paper appendix against installed Ceph 18.2.7 plugins."""
import argparse
import csv
import json
from math import comb
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import tempfile
import time

import reproduce as core

TSX = Path(__file__).resolve().parents[1]
CEPH = next((p for p in TSX.parents if (p/'src/erasure-code/tsx').resolve()==TSX), TSX)
BUILD = CEPH / 'build'
PLUGINS = ['tsx', 'tsx2', 'tsx4', 'tsxse', 'tsxnaive', 'tsxmap', 'tsxwin', 'tsxmerge']
ABLATIONS = ['tsxnaive', 'tsxmap', 'tsxwin', 'tsxmerge']
METRIC = re.compile(r'^\s*([0-9]+(?:\.[0-9]+)?)\s+([0-9]+)\s*$', re.M)


def execute(cmd, *, cwd=None, env=None, timeout=180, log=None):
    try:
        result = subprocess.run([str(x) for x in cmd], cwd=cwd, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        if log:
            text=error.stdout or ''
            Path(log).write_text(text.decode(errors='replace') if isinstance(text,bytes) else text)
        raise RuntimeError(f'Command timed out after {timeout}s: {cmd}; log={log}') from error
    if log:
        Path(log).write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {cmd}\n{result.stdout[-2500:]}')
    return result.stdout


def output_dir(kind, requested=None):
    if requested:
        path = Path(requested).resolve()
        path.mkdir(parents=True, exist_ok=False)
    else:
        root = CEPH / 'local_workspace/paper-reproduction'
        root.mkdir(parents=True, exist_ok=True)
        path = Path(tempfile.mkdtemp(prefix=kind+'-', dir=root))
    path.chmod(0o755)
    print('OUTPUT_DIRECTORY:',path,flush=True)
    return path


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False)+'\n')


def plugin_dir(args):
    if args.plugin_dir:
        return args.plugin_dir.resolve()
    prefix = Path(core.read_cache(BUILD).get('CMAKE_INSTALL_PREFIX', '/usr/local'))
    return prefix / 'lib/ceph/erasure-code'


def manifest(kind, plugins):
    return {'kind': kind, 'ceph_head': core.git(CEPH, 'rev-parse', 'HEAD'),
            'tsx_head': core.git(TSX, 'rev-parse', 'HEAD'),
            'tsx_status': core.git(TSX, 'status', '--short'),
            'installed_plugins': {p.name: core.sha256(p) for p in plugins.glob('libec_*.so')},
            'plugin_directory': str(plugins),
            'runner_sha256': core.sha256(Path(__file__)),
            'note': 'Different hardware/results are permitted; upstream ISA-L runtime SIMD dispatch is recorded, not forced to AVX2.',
            'records': [], 'status': 'running'}


def install(args):
    core.configure(CEPH, BUILD, args.jobs)
    targets = core.TARGETS + ['ec_'+x for x in PLUGINS if 'ec_'+x not in core.TARGETS]
    targets += ['erasure_code_plugins']
    if args.cluster:
        targets += ['vstart']
    core.run(['cmake', '--build', BUILD, '--parallel', args.jobs, '--target', *targets])
    # Install only the generated EC subtree, without a global ninja install.
    core.run(['cmake', '-P', BUILD/'src/erasure-code/cmake_install.cmake'])
    plugins = plugin_dir(args)
    for name in PLUGINS + ['isa', 'jerasure']:
        path = plugins/f'libec_{name}.so'
        if not path.is_file():
            raise RuntimeError('Missing installed plugin: '+str(path))
        linked = execute(['ldd', path])
        if 'not found' in linked:
            raise RuntimeError('Unresolved installed plugin dependency: '+linked)
    core.report(CEPH, BUILD, 'fasttt-build.json', status='passed')
    data = manifest('plugin-installation', plugins)
    data.update(status='passed', cluster_targets_built=args.cluster,
                install_script=str(BUILD/'src/erasure-code/cmake_install.cmake'))
    save(BUILD/'fasttt-installation.json', data)
    print('Installed and dependency-checked:', plugins, flush=True)


def selections(k, m, seed):
    rng = random.Random(seed)
    for count in range(1, m+1):
        for data in range(count, -1, -1):
            parity = count-data
            yield data, parity, sorted(rng.sample(range(k), data)+rng.sample(range(k,k+m), parity))


def benchmark_command(plugins, plugin, k, m, chunk, iterations, workload, erased, pattern):
    cmd = [BUILD/'bin/ceph_erasure_code_benchmark', '--erasure-code-dir', plugins,
           '-p', plugin, '-P', f'k={k}', '-P', f'm={m}', '-s', str(k*chunk),
           '-i', str(iterations), '-w', workload, '--input-pattern', pattern, '--input-seed', '1']
    if plugin in ('isa','jerasure'):
        cmd += ['-P', 'technique='+('cauchy' if plugin=='isa' else 'cauchy_good')]
    if workload=='decode':
        cmd += ['-e', str(len(erased))]
        for index in erased:
            cmd += ['--erased', str(index)]
    return cmd


def coding(args):
    core.check_sources(CEPH); core.check_build(CEPH, BUILD)
    plugins = plugin_dir(args)
    out = output_dir('coding', args.output)
    data = manifest('appendix-A1-coding-and-ablation', plugins)
    cpu = args.cpu if args.cpu is not None else min(os.sched_getaffinity(0))
    env = os.environ.copy(); env['OMP_NUM_THREADS']='1'
    data.update(iterations_per_case=args.iterations, chunk_bytes=args.chunk_kib*1024,
                repeats=args.repeats, cpu=cpu, input_pattern=args.pattern, seed=args.seed,
                averaging='arithmetic mean of per-case throughput within each total erasure count; per-case raw timings retained')
    save(out/'results.json', data)
    configs = [(args.k,args.m)] if args.k is not None else [(6,2),(6,3),(8,3),(10,4)]
    try:
        for k,m in configs:
            fast = {2:'tsx2',3:'tsx',4:'tsx4'}[m]
            names = [fast,'isa','jerasure'] + (ABLATIONS if (k,m)==(8,3) and not args.no_ablation else [])
            cases = [('encode',0,0,[])] + [('decode',d,p,erased) for d,p,erased in selections(k,m,args.seed)]
            for plugin in names:
                if not (plugins/f'libec_{plugin}.so').is_file():
                    raise RuntimeError('Install plugin first: '+plugin)
                for workload,d,p,erased in cases:
                    warm = benchmark_command(plugins,plugin,k,m,args.chunk_kib*1024,2,workload,erased,args.pattern)
                    execute(['taskset','-c',str(cpu),*warm],env=env)
                    for repeat in range(args.repeats):
                        cmd = benchmark_command(plugins,plugin,k,m,args.chunk_kib*1024,args.iterations,workload,erased,args.pattern)
                        name=f'{k}-{m}-{plugin}-{workload}-{d}d-{p}p-r{repeat}'
                        text=execute(['taskset','-c',str(cpu),*cmd],env=env,log=out/(name+'.log'))
                        metrics=METRIC.findall(text)
                        if len(metrics)!=1:
                            raise RuntimeError('Ambiguous benchmark output: '+name)
                        seconds,kib=float(metrics[0][0]),int(metrics[0][1])
                        if seconds<=0 or kib!=args.iterations*(k*args.chunk_kib):
                            raise RuntimeError('Invalid measured time/bytes: '+name)
                        data['records'].append({'k':k,'m':m,'plugin':plugin,'workload':workload,
                            'data_erasures':d,'parity_erasures':p,'erased':erased,'repeat':repeat,
                            'seconds':seconds,'input_kib':kib,'gib_per_s':kib/1048576/seconds,
                            'gb_per_s':kib*1024/1e9/seconds,'command':[str(x) for x in cmd]})
                        save(out/'results.json',data)
                        print(name, 'PASS', flush=True)
        data['status']='passed'
    except Exception as error:
        data.update(status='failed', error=str(error)); raise
    finally:
        save(out/'results.json',data)
    with (out/'raw.csv').open('w') as handle:
        fields=['k','m','plugin','workload','data_erasures','parity_erasures','repeat','seconds','gib_per_s','gb_per_s']
        writer=csv.DictWriter(handle,fieldnames=fields);writer.writeheader()
        for r in data['records']:writer.writerow({f:r[f] for f in fields})
    grouped={}
    for r in data['records']:
        key=(r['k'],r['m'],r['plugin'],r['workload'],r['data_erasures']+r['parity_erasures'])
        grouped.setdefault(key,[]).append(r['gib_per_s'])
    with (out/'summary.csv').open('w') as handle:
        writer=csv.writer(handle);writer.writerow(['k','m','plugin','workload','erasures','mean_gib_per_s','measurements'])
        for key,values in grouped.items():writer.writerow([*key,sum(values)/len(values),len(values)])
    print('RESULT:',out,flush=True)


def verify_installed(args):
    core.check_sources(CEPH);core.check_build(CEPH,BUILD)
    plugins=plugin_dir(args);out=output_dir('installed-correctness',args.output)
    data=manifest('installed-plugin-exhaustive-random-correctness',plugins)
    try:
        for k,m in [(6,2),(6,3),(8,3),(10,4)]:
            names=[{2:'tsx2',3:'tsx',4:'tsx4'}[m],'isa','jerasure']+(ABLATIONS if (k,m)==(8,3) else [])
            for plugin in names:
                for count in range(1,m+1):
                    cmd=[BUILD/'bin/ceph_erasure_code_benchmark','--erasure-code-dir',plugins,
                         '-p',plugin,'-P',f'k={k}','-P',f'm={m}','-s',str(k*2097152),
                         '-i','1','-w','decode','-E','exhaustive','-e',str(count),'-v',
                         '--input-pattern','random','--input-seed','1']
                    if plugin in ('isa','jerasure'):cmd+=['-P','technique='+('cauchy' if plugin=='isa' else 'cauchy_good')]
                    label=f'{k}-{m}-{plugin}-e{count}'
                    text=execute(cmd,log=out/(label+'.log'))
                    attempted=sum(line.startswith('chunks ') for line in text.splitlines())
                    if attempted!=comb(k+m,count):raise RuntimeError('Incomplete exhaustive coverage: '+label)
                    data['records'].append({'k':k,'m':m,'plugin':plugin,'erasures':count,
                                            'combinations':attempted,'command':[str(x) for x in cmd]})
                    save(out/'results.json',data);print(label,'PASS',flush=True)
        data['status']='passed'
    except Exception as error:
        data.update(status='failed',error=str(error));raise
    finally:save(out/'results.json',data)
    print('RESULT:',out,flush=True)


def cluster_processes(conf):
    found=[]
    for path in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            words=path.read_bytes().split(b'\0')
            if words and Path(os.fsdecode(words[0])).name in ('ceph-mon','ceph-osd') and os.fsencode(str(conf)) in words:
                found.append((int(path.parent.name),[os.fsdecode(x) for x in words if x]))
        except (FileNotFoundError,PermissionError,ProcessLookupError):
            pass
    return found


def cerasure(args):
    import shutil
    core.check_sources(CEPH)
    root=args.cerasure_root.resolve()
    pin=core.LOCK['cerasure']
    if not root.is_dir():
        raise RuntimeError('Clone '+pin['repository']+' --branch fasttt into '+str(root))
    if core.git(root,'rev-parse','HEAD')!=pin['commit']:
        raise RuntimeError('Cerasure HEAD must be '+pin['commit'])
    if args.iterations!=2000:raise RuntimeError('Original A2 fasttt.sh fixes -n 2000; use --iterations 2000')
    out=output_dir('cerasure',args.output)
    deps=CEPH/'local_workspace/cerasure-deps'
    source=deps/'gf-complete';prefix=deps/'install'
    if not source.exists():
        shutil.copytree(CEPH/'src/erasure-code/jerasure/gf-complete',source,ignore=shutil.ignore_patterns('.git'))
    env=os.environ.copy()
    env.update(CFLAGS='-mavx2 -O3',CPPFLAGS='-I'+str(prefix/'include'),
               LDFLAGS=f'-L{prefix}/lib -Wl,-rpath,{prefix}/lib -Wl,--allow-multiple-definition',
               LD_LIBRARY_PATH=str(prefix/'lib')+':'+env.get('LD_LIBRARY_PATH',''),OMP_NUM_THREADS='1')
    if not (prefix/'lib/libgf_complete.so').exists():
        execute(['autoreconf','-fi'],cwd=source,env=env,log=out/'gf-autoreconf.log')
        execute(['./configure','--prefix='+str(prefix)],cwd=source,env=env,log=out/'gf-configure.log')
        execute(['make','-j'+str(args.jobs)],cwd=source,env=env,timeout=600,log=out/'gf-make.log')
        execute(['make','install'],cwd=source,env=env,log=out/'gf-install.log')
    execute(['./autogen.sh'],cwd=root,env=env,log=out/'autogen.log')
    execute(['./configure','--prefix='+str(root/'install')],cwd=root,env=env,log=out/'configure.log')
    execute(['make','-j'+str(args.jobs)],cwd=root,env=env,timeout=600,log=out/'make.log')
    execute(['make','ex','-j'+str(args.jobs)],cwd=root,env=env,timeout=600,log=out/'make-ex.log')
    cpu=args.cpu if args.cpu is not None else min(os.sched_getaffinity(0))
    command=['taskset','-c',str(cpu),'bash','-e','fasttt.sh']
    text=execute(command,cwd=root,env=env,timeout=1200,log=out/'fasttt.log')
    labels=list(re.finditer(r'^\[(cerasure|tsx)\] k=(\d+), p=(\d+), loss=(\d+)\s*$',text,re.M))
    records=[]
    for i,label in enumerate(labels):
        block=text[label.end():labels[i+1].start() if i+1<len(labels) else len(text)]
        if 'decode success' not in block or 'decode fail' in block:raise RuntimeError('A2 decode check failed; see '+str(out/'fasttt.log'))
        numbers=re.findall(r'^\s*([0-9]+\.[0-9]+)\s*$',block,re.M)
        if len(numbers)<2:raise RuntimeError('Missing A2 encode/decode rates')
        records.append({'implementation':label[1],'k':int(label[2]),'m':int(label[3]),'erasures':int(label[4]),
                        'encode_rate_as_printed':float(numbers[-2]),'decode_rate_as_printed':float(numbers[-1]),'decode_matches':True})
    if len(records)!=24:raise RuntimeError('A2 expected 24 original-script measurements, got '+str(len(records)))
    data={'status':'passed','cerasure':pin,'gf_complete_commit':core.git(CEPH,'rev-parse','HEAD:src/erasure-code/jerasure/gf-complete'),
          'command':command,'cpu':cpu,'iterations_per_measurement':2000,'records':records,
          'rate_unit':'GiB/s; original C code divides bytes by 1024^3',
          'executable_sha256':core.sha256(root/'raid/.libs/xor_example'),
          'script_sha256':core.sha256(root/'fasttt.sh'),'runner_sha256':core.sha256(Path(__file__))}
    save(out/'results.json',data)
    with (out/'summary.csv').open('w') as handle:
        writer=csv.DictWriter(handle,fieldnames=list(records[0]));writer.writeheader();writer.writerows(records)
    print('RESULT:',out,flush=True)


def end_to_end(args):
    core.check_sources(CEPH); core.check_build(CEPH,BUILD)
    plugins=plugin_dir(args)
    for name in ['ceph-mon','ceph-osd','rados','monmaptool','ceph-conf','ceph-authtool']:
        if not (BUILD/'bin'/name).is_file():raise RuntimeError('Build install --cluster first: '+name)
    out=output_dir('end-to-end',args.output)
    state=Path(tempfile.mkdtemp(prefix='fasttt-paper-cluster-',dir='/tmp'))
    state.chmod(0o755)
    conf=state/'ceph.conf'
    configs=[(args.k,args.m)] if args.k is not None else [(6,2),(6,3),(8,3),(10,4)]
    osds=max(k+m for k,m in configs)
    data=manifest('appendix-A1-vstart-bluestore-put-get',plugins)
    data.update(cluster_directory=str(state),osds=osds,backend='bluestore',
                storage='regular files inside dedicated container /tmp, not raw block devices; not MemStore',
                put_iterations=args.put_iterations,stripes=args.stripes,degraded_read=args.degraded_read,
                status='starting')
    save(out/'results.json',data)
    env=os.environ.copy()
    env.update(VSTART_DEST=str(state),CEPH_BIN=str(BUILD/'bin'),CEPH_LIB=str(BUILD/'lib'),
               EC_PATH=str(plugins),MON='1',OSD=str(osds),MDS='0',MGR='0',FS='0',RGW='0',NFS='0',
               CEPH_PORT='43100',CEPH_DEV='1',
               PYTHONPATH=':'.join(map(str,[CEPH/'src/pybind',BUILD/'lib/cython_modules/lib.3',CEPH/'src/python-common'])),
               LD_LIBRARY_PATH=str(BUILD/'lib')+':'+env.get('LD_LIBRARY_PATH',''))
    def ceph(*items,json_output=False,timeout=60):
        cmd=[BUILD/'bin/ceph','-c',conf,*items]
        if json_output:cmd+=['-f','json']
        text=execute(cmd,env=env,timeout=timeout)
        return json.loads(text) if json_output else text
    def rados(*items,timeout=180):
        return execute([BUILD/'bin/rados','-c',conf,*items],env=env,timeout=timeout)
    def wait_up():
        deadline=time.monotonic()+120
        while time.monotonic()<deadline:
            dump=ceph('osd','dump',json_output=True)
            if len(dump['osds'])==osds and all(x['up'] for x in dump['osds']):return
            time.sleep(1)
        raise RuntimeError('OSDs failed to become up')
    try:
        cmd=['bash',CEPH/'src/vstart.sh','-n','-X','-l','--bluestore','--without-dashboard',
             '-o','bluestore block size = 2147483648',
             '-o','bluestore block db size = 268435456',
             '-o','bluestore block wal size = 67108864',
             '-o','osd pool default pg autoscale mode = off']
        data['vstart_command']=[str(x) for x in cmd]
        execute(cmd,cwd=BUILD,env=env,timeout=600,log=out/'vstart.log')
        wait_up()
        data['fsid']=ceph('fsid').strip()
        data['status']='running'
        ceph('osd','set','noout')
        save(out/'initial-osd-map.json',ceph('osd','dump',json_output=True))
        (out/'ceph.conf').write_text(conf.read_text())
        for k,m in configs:
            source=out/f'input-{k}-{m}.bin'
            size=k*args.chunk_kib*1024*args.stripes
            if size>120*1024*1024:raise RuntimeError('Use <=120MiB objects for this minimal workflow')
            # The published codec keys Redis tails by content without a profile
            # namespace. Distinct deterministic inputs keep k/m experiments apart.
            effective_seed=args.seed*1000+k*10+m
            rng=random.Random(effective_seed)
            with source.open('wb') as handle:
                remaining=size
                while remaining:
                    block=min(remaining,1048576);handle.write(rng.randbytes(block));remaining-=block
            expected=core.sha256(source)
            for plugin in [{2:'tsx2',3:'tsx',4:'tsx4'}[m],'isa','jerasure']:
                name=f'paper-{k}-{m}-{plugin}';profile=name+'-profile'
                values=['plugin='+plugin,f'k={k}',f'm={m}','crush-failure-domain=osd',f'stripe_unit={args.chunk_kib*1024}']
                # The appendix's vstart profile leaves technique at its default:
                # reed_sol_van for both official plugins. Its memory benchmark
                # separately uses cauchy/cauchy_good.
                if plugin in ('isa','jerasure'):values+=['technique=reed_sol_van']
                ceph('osd','erasure-code-profile','set',profile,*values)
                ceph('osd','pool','create',name,'8','8','erasure',profile)
                ceph('osd','pool','application','enable',name,'rados')
                record={'k':k,'m':m,'plugin':plugin,'pool':name,'bytes':size,'sha256':expected,'data_seed':effective_seed,
                        'profile':ceph('osd','erasure-code-profile','get',profile,json_output=True),'puts':[]}
                data['records'].append(record)
                for index in range(args.put_iterations):
                    obj=f'paper-object-{index}'
                    started=time.monotonic();rados('-p',name,'put',obj,source);seconds=time.monotonic()-started
                    received=out/f'{name}-get.bin'
                    rados('-p',name,'get',obj,received)
                    if core.sha256(received)!=expected:raise RuntimeError('PUT/GET mismatch: '+name)
                    record['puts'].append({'object':obj,'seconds':seconds,'mib_per_s':size/1048576/seconds,'get_matches':True})
                    received.unlink()
                    save(out/'results.json',data)
                    print(name,'PUT/GET',index+1,'PASS',flush=True)
                if args.degraded_read:
                    obj=record['puts'][-1]['object']
                    mapping=ceph('osd','map',name,obj,json_output=True)
                    victim=mapping['acting'][0]
                    own=cluster_processes(conf)
                    candidates=[(pid,words) for pid,words in own if 'ceph-osd' in words[0] and any(words[i] in ('-i','--id') and words[i+1]==str(victim) for i in range(len(words)-1))]
                    if len(candidates)!=1:raise RuntimeError('Cannot identify exact test OSD PID')
                    pid=candidates[0][0]
                    os.kill(pid,signal.SIGTERM)
                    deadline=time.monotonic()+45
                    while any(p==pid for p,_ in cluster_processes(conf)) and time.monotonic()<deadline:time.sleep(0.5)
                    if any(p==pid for p,_ in cluster_processes(conf)):raise RuntimeError('Test OSD did not stop cleanly')
                    try:
                        ceph('osd','down',str(victim))
                        received=out/f'{name}-degraded.bin'
                        rados('-p',name,'get',obj,received)
                        if core.sha256(received)!=expected:raise RuntimeError('Degraded GET mismatch: '+name)
                        received.unlink();record['degraded_read']={'stopped_osd':victim,'sha256_matches':True}
                        print(name,'degraded GET PASS',flush=True)
                    finally:
                        log=(out/f'osd-{victim}-restart.log').open('a')
                        subprocess.Popen([str(BUILD/'bin/ceph-osd'),'-f','-i',str(victim),'-c',str(conf)],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
                        log.close();wait_up()
                record['mean_put_seconds']=sum(x['seconds'] for x in record['puts'])/len(record['puts'])
                record['mean_put_mib_per_s']=size/1048576/record['mean_put_seconds']
                save(out/'results.json',data)
        data['status']='passed'
    except Exception as error:
        data.update(status='failed',error=str(error));raise
    finally:
        # Filter by both executable and the exact per-run config, never global pkill.
        forced=[]
        # Let OSDs finish while their monitor is still available.
        for role in ('ceph-osd','ceph-mon'):
            own=lambda: [(pid,words) for pid,words in cluster_processes(conf) if Path(words[0]).name==role]
            for pid,_ in own():
                try:os.kill(pid,signal.SIGTERM)
                except ProcessLookupError:pass
            deadline=time.monotonic()+30
            while own() and time.monotonic()<deadline:time.sleep(0.5)
            for pid,_ in own():
                try:os.kill(pid,signal.SIGKILL);forced.append(pid)
                except ProcessLookupError:pass
        data['cleanup']={'exact_config_only':str(conf),'forced_pids':forced}
        save(out/'results.json',data)
        if (state/'out').exists():
            import shutil
            shutil.copytree(state/'out',out/'daemon-logs',dirs_exist_ok=True)
    with (out/'summary.csv').open('w') as handle:
        writer=csv.writer(handle);writer.writerow(['k','m','plugin','object_bytes','mean_put_mib_per_s','puts','degraded_get_matches'])
        for r in data['records']:writer.writerow([r['k'],r['m'],r['plugin'],r['bytes'],r['mean_put_mib_per_s'],len(r['puts']),r.get('degraded_read',{}).get('sha256_matches')])
    print('RESULT:',out,flush=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['install','verify-installed','coding','end-to-end','cerasure'])
    parser.add_argument('--jobs',type=int,default=16)
    parser.add_argument('--cluster',action='store_true')
    parser.add_argument('--plugin-dir',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--k',type=int)
    parser.add_argument('--m',type=int)
    parser.add_argument('--chunk-kib',type=int,default=2048)
    parser.add_argument('--iterations',type=int,default=2000)
    parser.add_argument('--repeats',type=int,default=1)
    parser.add_argument('--seed',type=int,default=1)
    parser.add_argument('--cpu',type=int)
    parser.add_argument('--pattern',choices=['constant','random'],default='constant')
    parser.add_argument('--no-ablation',action='store_true')
    parser.add_argument('--put-iterations',type=int,default=3)
    parser.add_argument('--stripes',type=int,default=4)
    parser.add_argument('--degraded-read',action='store_true')
    parser.add_argument('--cerasure-root',type=Path,default=CEPH/'local_workspace/cerasure-paper')
    args=parser.parse_args()
    if (args.k is None)!=(args.m is None):parser.error('--k and --m must be supplied together')
    if args.k is not None and (args.k,args.m) not in [(6,2),(6,3),(8,3),(10,4)]:parser.error('Only paper configurations are supported')
    if min(args.jobs,args.chunk_kib,args.iterations,args.repeats,args.put_iterations,args.stripes)<1:parser.error('Counts must be positive')
    if args.action=='install':install(args)
    elif args.action=='verify-installed':verify_installed(args)
    elif args.action=='coding':coding(args)
    elif args.action=='end-to-end':end_to_end(args)
    else:cerasure(args)


if __name__=='__main__':
    main()
