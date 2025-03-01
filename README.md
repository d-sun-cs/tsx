# TSX (Tow-Tone Shift-XOR) Erasure Code Algorithm

TSX (Tow-Tone Shift-XOR) is an erasure code algorithm integrated into Ceph. This algorithm provides efficient data protection and recovery capabilities.

## Integration into Ceph

To integrate the TSX erasure code algorithm into Ceph, you need to modify the `CMakeLists.txt` file located in the `src/erasure-code` directory. Specifically, you should add the following lines:

```cmake
add_subdirectory(tsx)
```

Additionally, you need to include `ec_tsx` in the `add_custom_target` section:

```cmake
add_custom_target(ec_tsx)
```

## Reference

For more detailed information about the TSX algorithm, you can refer to the research paper available at the following link: [Tow-Tone Shift-XOR Algorithm](https://ieeexplore.ieee.org/document/9518205).
