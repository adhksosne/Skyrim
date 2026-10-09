// preinclude.h —— 通过 MSVC /FI 在每个翻译单元最前面强制预包含（见 CMakeLists.txt）
//
// 必要性：CommonLibSSE-NG 3.5.3 的 REL/Relocation.h 不自包含——它直接使用
//   stl:: / WinAPI:: 命名空间别名、std::span、std::array、operator""sv，
//   但自身仅 #include "rapidcsv.h"。这些符号全部由 SKSE/Impl/PCH.h
//   提供（该头包含全套标准库头，并在全局作用域提供 stl/WinAPI 别名与
//   using namespace std::literals）。add_commonlibsse_plugin 生成的
//   __SummonNameFixCNPlugin.cpp 第一行即 #include "REL/Relocation.h"，
//   无机会先包含 PCH.h，故用 /FI 补齐。
#include "SKSE/Impl/PCH.h"

// PCH.h 中的 literals 引入位于 namespace SKSE 内，此处于全局作用域再次引入，
// 供 REL 命名空间中的 operator""sv 使用（重复引入无副作用）。
using namespace std::literals;
