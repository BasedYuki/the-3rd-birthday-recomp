input_file = "map.txt"  # Guarda tus líneas aquí
output_file = "build/mygame/functions.toml"

base_address = None
functions = []

with open(input_file, "r") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        if line.startswith(".module"):
            parts = line.split()
            # Extrae la base address (tercer elemento: 08804040)
            if len(parts) >= 3:
                base_address = int(parts[2], 16)
            continue

        # Líneas de funciones: offset, tamaño, flags, etc., nombre
        parts = line.split()
        if len(parts) >= 5 and base_address is not None:
            offset = int(parts[0], 16)
            name = parts[-1]  # El nombre suele estar al final
            addr = base_address + offset
            functions.append({"addr": addr, "kind": "function", "name": name})

# Escribir el TOML correctamente con enteros puros
entry_point = base_address if base_address else 0x08804040

with open(output_file, "w") as f:
    f.write('module = "elf"\n')
    f.write(f'entry = {entry_point}\n\n')  # Guardar como entero decimal
    for fn in functions:
        f.write("[[function]]\n")
        f.write(f'addr = {fn["addr"]}\n')  # Guardar como entero decimal (ej: 142622784)
        f.write(f'kind = "{fn["kind"]}"\n')
        f.write(f'name = "{fn["name"]}"\n\n')

print(f"¡Generado {output_file} corregido con {len(functions)} funciones!")
