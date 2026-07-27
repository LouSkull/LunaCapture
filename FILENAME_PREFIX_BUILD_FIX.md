# Filename Prefix build fix

Fixed MSVC build failure in `RegionEditor.cpp`:

- added the missing `#include "CoreUtils.hpp"`;
- `SanitizeFilenamePrefix(...)` is now declared before use;
- the secondary ambiguous `std::wstring::operator=` diagnostic disappears with the primary declaration error;
- added an explicit `<cstddef>` include for `std::size_t` in `WinUtil.hpp`.

The visible application version remains `1.0.0.0`.
