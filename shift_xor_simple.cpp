/**
 * @filename: main.cpp
 * @desc: testing speed of shift-xor encoding
 * @author: Chenhao Wu (chenhaowu[at]link.cuhk.edu.cn)
 */

#include <iostream>
#include <fstream>
#include <sys/time.h>
#include <cstdlib>
#include <random>
#include <stdint.h>
#include <chrono>
#include <cstring>

/* add control flags on
 * shift units, packet size, and intrinsic sets */
#define FILE_SIZE (512 * 1024UL)
#define CHUNK_SIZE (FILE_SIZE / 8)
#define SLICE (8)
#define TOTAL_SLICE_NUM (CHUNK_SIZE / SLICE)
#define PARITY_PADDING (SLICE * 20)
#define PARITY_SIZE (CHUNK_SIZE + PARITY_PADDING)

#define SAMPLE_NUM 0x01UL
#define ENCODE_FILE "/tmp/largefile"

uint8_t input_chunk_1[CHUNK_SIZE] = {0};
uint8_t input_chunk_2[CHUNK_SIZE] = {0};
uint8_t input_chunk_3[CHUNK_SIZE] = {0};
uint8_t input_chunk_4[CHUNK_SIZE] = {0};
uint8_t input_chunk_5[CHUNK_SIZE] = {0};
uint8_t input_chunk_6[CHUNK_SIZE] = {0};
uint8_t input_chunk_7[CHUNK_SIZE] = {0};
uint8_t input_chunk_8[CHUNK_SIZE] = {0};
uint8_t output_chunk_6[CHUNK_SIZE] = {0};
uint8_t output_chunk_7[CHUNK_SIZE] = {0};
uint8_t output_chunk_8[CHUNK_SIZE] = {0};
uint8_t parity_packet_1[PARITY_SIZE] = {0};
uint8_t parity_packet_2[PARITY_SIZE] = {0};
uint8_t parity_packet_3[PARITY_SIZE] = {0};

void encode()
{
    uint64_t i;
    uint64_t slice_offset_1;
    uint64_t slice_offset_2;
    uint64_t slice_offset_3;
    /* generating matrix:
     * p1  p2    p3    p4    p5    p6     p7     p8
     * 1 + 1   + 1   + 1   + 1   + 1    + 1    + 1
     * 1 + z^1 + z^2 + z^3 + z^4 + z^5  + z^6  + z^7
     * 1 + z^2 + z^4 + z^6 + z^8 + z^10 + z^12 + z^14
     * */
    /* encode using input packet 1 */
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = slice_id * SLICE;
        slice_offset_3 = ((slice_id + 7) * SLICE);
        *(uint64_t*)(parity_packet_1 + slice_offset_1) = *(uint64_t*)(input_chunk_1 + slice_offset_2);
        *(uint64_t*)(parity_packet_2 + slice_offset_2) = *(uint64_t*)(input_chunk_1 + slice_offset_2);
        *(uint64_t*)(parity_packet_3 + slice_offset_3) = *(uint64_t*)(input_chunk_1 + slice_offset_2);
    }

    /* encode using input packet 2 */
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = ((slice_id + 1) * SLICE);
        slice_offset_3 = ((slice_id + 6) * SLICE);
        *(uint64_t*)(parity_packet_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_2 + slice_offset_2);
        *(uint64_t*)(parity_packet_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_2 + slice_offset_2);
        *(uint64_t*)(parity_packet_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_2 + slice_offset_2);
    }

	/* encode using input packet 3 */
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = ((slice_id + 2) * SLICE);
        slice_offset_3 = ((slice_id + 5) * SLICE);
        *(uint64_t*)(parity_packet_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_3 + slice_offset_2);
        *(uint64_t*)(parity_packet_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_3 + slice_offset_2);
        *(uint64_t*)(parity_packet_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_3 + slice_offset_2);
    }

    /* encode using input packet 4 */
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = ((slice_id + 3) * SLICE);
        slice_offset_3 = ((slice_id + 4) * SLICE);
        *(uint64_t*)(parity_packet_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_4 + slice_offset_2);
        *(uint64_t*)(parity_packet_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_4 + slice_offset_2);
        *(uint64_t*)(parity_packet_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_4 + slice_offset_2);
    }

    /* encode using input packet 5 */
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = ((slice_id + 4) * SLICE);
        slice_offset_3 = ((slice_id + 3) * SLICE);
        *(uint64_t*)(parity_packet_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_5 + slice_offset_2);
        *(uint64_t*)(parity_packet_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_5 + slice_offset_2);
        *(uint64_t*)(parity_packet_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_5 + slice_offset_2);
    }

    /* encode using input packet 6 */
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = ((slice_id + 5) * SLICE);
        slice_offset_3 = ((slice_id + 2) * SLICE);
        *(uint64_t*)(parity_packet_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_6 + slice_offset_2);
        *(uint64_t*)(parity_packet_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_6 + slice_offset_2);
        *(uint64_t*)(parity_packet_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_6 + slice_offset_2);
    }

    /* encode using input packet 7 */
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = ((slice_id + 6) * SLICE);
        slice_offset_3 = ((slice_id + 1) * SLICE);
        *(uint64_t*)(parity_packet_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_7 + slice_offset_2);
        *(uint64_t*)(parity_packet_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_7 + slice_offset_2);
        *(uint64_t*)(parity_packet_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_7 + slice_offset_2);
    }

    /* encode using input packet 8 */
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = ((slice_id + 7) * SLICE);
        slice_offset_3 = slice_id * SLICE;
        *(uint64_t*)(parity_packet_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_8 + slice_offset_2);
        *(uint64_t*)(parity_packet_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_8 + slice_offset_2);
        *(uint64_t*)(parity_packet_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_8 + slice_offset_2);
    }
}

void decode() 
{
    // assume that only input chunk 1-5 and parity file 1-3 are available
    uint64_t slice_offset_1;
    uint64_t slice_offset_2;
    uint64_t slice_offset_3;

    // back substitution for packet 1-5
    uint8_t* decode_arr_1 = (uint8_t*) malloc(PARITY_SIZE * sizeof(uint8_t));
    uint8_t* decode_arr_2 = (uint8_t*) malloc(PARITY_SIZE * sizeof(uint8_t));
    uint8_t* decode_arr_3 = (uint8_t*) malloc(PARITY_SIZE * sizeof(uint8_t));
    memcpy(decode_arr_1, parity_packet_1, PARITY_SIZE);
    memcpy(decode_arr_2, parity_packet_2, PARITY_SIZE);
    memcpy(decode_arr_3, parity_packet_3, PARITY_SIZE);

    // back substituite packet 1
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) 
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = slice_id * SLICE;
        slice_offset_3 = (slice_id + 7) * SLICE;

        *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_1 + slice_offset_2);
        *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_1 + slice_offset_2);
        *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_1 + slice_offset_2);
    }

    // back substitute packet 2
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) 
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = (slice_id + 1) * SLICE;
        slice_offset_3 = (slice_id + 6) * SLICE;

        *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_2 + slice_offset_2);
        *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_2 + slice_offset_2);
        *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_2 + slice_offset_2);
    }

    // back substitute packet 3
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) 
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = (slice_id + 2) * SLICE;
        slice_offset_3 = (slice_id + 5) * SLICE;

        *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_3 + slice_offset_2);
        *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_3 + slice_offset_2);
        *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_3 + slice_offset_2);
    }

    // back substitute packet 4
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) 
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = (slice_id + 3) * SLICE;
        slice_offset_3 = (slice_id + 4) * SLICE;

        *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_4 + slice_offset_2);
        *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_4 + slice_offset_2);
        *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_4 + slice_offset_2);
    }

    // back substitute packet 5
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++) 
    {
        slice_offset_2 = slice_id * SLICE;
        slice_offset_1 = (slice_id + 4) * SLICE;
        slice_offset_3 = (slice_id + 3) * SLICE;

        *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(input_chunk_5 + slice_offset_2);
        *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(input_chunk_5 + slice_offset_2);
        *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(input_chunk_5 + slice_offset_2);
    }

    // solve chunk 6,7,8
    for (int slice_id = 0; slice_id < TOTAL_SLICE_NUM; slice_id++)
    {
        // Step 1: solve 64 bits in packet 6
        slice_offset_1 = ((slice_id + 5) * SLICE);
        slice_offset_2 = slice_id * SLICE;
        slice_offset_3 = ((slice_id) + 2) * SLICE;

        // directly use leftmost block of parity packet 1 as message of packet 6
        *(uint64_t*)(output_chunk_6 + slice_offset_2) = *(uint64_t*)(decode_arr_1 + slice_offset_1);

        // back substitute packet 6 to parity packet 2 and 3
        *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(decode_arr_1 + slice_offset_1);
        *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(decode_arr_1 + slice_offset_1);

        // Step 2: solve 64 bits in packet 8
        slice_offset_1 = ((slice_id + 7) * SLICE);
        slice_offset_2 = slice_id * SLICE;
        slice_offset_3 = slice_id * SLICE;

        // directly use leftmost block of parity packet 3 as message of packet 8
        *(uint64_t*)(output_chunk_8 + slice_offset_2) = *(uint64_t*)(decode_arr_3 + slice_offset_3);

        // back substitute packet 8 to parity 1 and 2
        *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(decode_arr_3 + slice_offset_3);
        *(uint64_t*)(decode_arr_2 + slice_offset_2) ^= *(uint64_t*)(decode_arr_3 + slice_offset_3);

        // Step 3: solve 64 bits in packet 7
        slice_offset_1 = ((slice_id + 6) * SLICE);
        slice_offset_2 = slice_id * SLICE;
        slice_offset_3 = ((slice_id + 1) * SLICE);

        // directly use leftmost block of parity packet 2 as message of packet 7
        *(uint64_t*)(output_chunk_7 + slice_offset_2) = *(uint64_t*)(decode_arr_2 + slice_offset_2);

        // back substitute packet 7 to parity packet 1 and 3
        *(uint64_t*)(decode_arr_1 + slice_offset_1) ^= *(uint64_t*)(decode_arr_2 + slice_offset_2);
        *(uint64_t*)(decode_arr_3 + slice_offset_3) ^= *(uint64_t*)(decode_arr_2 + slice_offset_2);
    }
    free(decode_arr_1);
    free(decode_arr_2);
    free(decode_arr_3);
}

void validate_decoding_result()
{
    if (strcmp((char *) output_chunk_6, (char *) input_chunk_6) == 0
        && strcmp((char *) output_chunk_7, (char *) input_chunk_7) == 0
        && strcmp((char *) output_chunk_8, (char *) input_chunk_8) == 0)
    {
        printf("output files are equal!\n");
    }
    else
    {
        printf("Error: output files are not equal!\n");
    }
}

int main()
{
    printf("start encoding\n");
    printf("input file size: %ld byte\n", FILE_SIZE);
    printf("chunk size: %ld byte\n", CHUNK_SIZE);

    double tic, toc;
    double exec_time_in_sec, throughput_in_MBps;
    std::fstream FileIn(ENCODE_FILE, std::ios::in | std::ios::out | std::ios::binary);
    if (FileIn.is_open())
    {
        FileIn.read((char *) input_chunk_1, CHUNK_SIZE);
        FileIn.read((char *) input_chunk_2, CHUNK_SIZE);
        FileIn.read((char *) input_chunk_3, CHUNK_SIZE);
        FileIn.read((char *) input_chunk_4, CHUNK_SIZE);
        FileIn.read((char *) input_chunk_5, CHUNK_SIZE);
        FileIn.read((char *) input_chunk_6, CHUNK_SIZE);
        FileIn.read((char *) input_chunk_7, CHUNK_SIZE);
        FileIn.read((char *) input_chunk_8, CHUNK_SIZE);
        FileIn.close();
        printf("file %s have been load into memory\n", ENCODE_FILE);
        printf("start encoding......\n");
        tic = clock();
        for (uint32_t i = 0; i < SAMPLE_NUM; i++)
            encode();
        toc = clock();

        exec_time_in_sec = (toc - tic) / (CLOCKS_PER_SEC / 1000);
        throughput_in_MBps = (SAMPLE_NUM * (FILE_SIZE + 3 * CHUNK_SIZE)) / (exec_time_in_sec * 1024);
        printf("encoding finished!\n");
        printf("throughput: %.4f MBps\n", throughput_in_MBps);

        printf("start decoding......\n");
        tic = clock();
        for (uint32_t i = 0; i < SAMPLE_NUM; i++)
            decode();
        toc = clock();
        exec_time_in_sec = (toc - tic) / (CLOCKS_PER_SEC / 1000);
        throughput_in_MBps = SAMPLE_NUM * (FILE_SIZE) / (exec_time_in_sec * 1024);
        printf("decoding finished!\n");
        printf("throughput: %.4f MBps\n", throughput_in_MBps);

        validate_decoding_result();

        printf("storing encoded result to files......\n");
    } else
    {
        printf("cannot open file dummy.txt. exit.\n");
        return 255;
    }
    printf("all subroutine of encoding have finished. exit.\n");
    return 0;
    return 0;
}
