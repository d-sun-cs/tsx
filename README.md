# TSX (Tow-Tone Shift-XOR) Erasure Code Algorithm

TSX (Tow-Tone Shift-XOR) is an erasure code algorithm integrated into Ceph. This algorithm provides efficient data protection and recovery capabilities.

## Integration into Ceph

To integrate the TSX erasure code algorithm into Ceph, you need to place the repository folder in the `/src/erasure-code` directory, with the repository name itself being `tsx`.

First, navigate to the `erasure-code` directory and clone the repository:

```sh
cd ceph/src/erasure-code
git clone https://github.com/d-sun-cs/tsx.git
```

Next, you need to modify the `CMakeLists.txt` file located in the `src/erasure-code` directory. Specifically, you should add the following lines:

```cmake
add_subdirectory(tsx)
```

Additionally, you need to include `ec_tsx` in the `add_custom_target` section:

```cmake
add_custom_target(ec_tsx)
```

## Local Compilation

To locally compile the new erasure code, navigate to the `ceph/build` directory and execute the following command:

```sh
cd ceph/build
ninja vstart
```

This will compile the new erasure code and allow you to start the vstart test cluster for testing.

## Reference

For more detailed information about the TSX algorithm, you can refer to the research paper available at the following link: [Two-tone Shift-XOR Storage Codes](https://ieeexplore.ieee.org/document/9518205).
