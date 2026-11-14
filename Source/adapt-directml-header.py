import sys
from pathlib import Path

if len(sys.argv) != 3:
    raise SystemExit("Usage: python adapt-directml-header.py PATH_TO_OFFICIAL_DIRECTML_H OUTPUT_HEADER")

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
original = "    IFACEMETHOD_(DML_BINDING_PROPERTIES, GetBindingProperties)() = 0;"
replacement = """#if defined(__MINGW32__) && defined(__x86_64__)
    // Windows COM ABI: this goes in RCX, the aggregate return storage goes in RDX.
    virtual DML_BINDING_PROPERTIES* STDMETHODCALLTYPE GetBindingProperties(
        DML_BINDING_PROPERTIES* result) = 0;

    DML_BINDING_PROPERTIES GetBindingProperties()
    {
        DML_BINDING_PROPERTIES result{};
        GetBindingProperties(&result);
        return result;
    }
#else
""" + original + "\n#endif"
if source.count(original) != 1:
    raise SystemExit("The supplied DirectML header does not have the expected method declaration")

Path(sys.argv[2]).write_text(source.replace(original, replacement), encoding="utf-8")
