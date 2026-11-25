import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_bytes()
destination = pathlib.Path(sys.argv[2])

with destination.open("w", encoding="ascii") as output:
    output.write("const unsigned char folio_font[] = {\n")
    for offset in range(0, len(source), 16):
        row = ", ".join(f"0x{byte:02x}" for byte in source[offset : offset + 16])
        output.write(f"    {row},\n")
    output.write("};\n")
    output.write("const int folio_font_size = sizeof folio_font;\n")
