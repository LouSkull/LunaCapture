# Region Color + Eraser build fix

Fixed MSVC error C2137 in `src/RegionEditor.cpp`.

The label check contained an embedded NUL byte inside a wide character literal. It is now the valid C++ literal:

```cpp
if (labels[i][0] != L'\0') {
```

All C++ source and header files were scanned for embedded NUL bytes after the repair.
