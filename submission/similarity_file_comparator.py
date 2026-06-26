import sys

def load_sim_file(path):
    data = set()
    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            parts = line.split("\t")
            if len(parts) != 3:
                raise ValueError(f"Bad line in {path}: {line}")

            i, j, sim = parts[0], parts[1], parts[2]

            # normalize types for stable comparison
            i = int(i)
            j = int(j)
            sim = float(sim)

            data.add((i, j, round(sim, 6)))  # match your output precision

    return data


def main(file1, file2):
    a = load_sim_file(file1)
    b = load_sim_file(file2)

    only_in_a = a - b
    only_in_b = b - a

    if not only_in_a and not only_in_b:
        print("OK: files match exactly")
        return 0

    print("Mismatch found!\n")

    if only_in_a:
        print("In file1 but not file2:")
        for x in sorted(only_in_a):
            print(x)

    if only_in_b:
        print("\nIn file2 but not file1:")
        for x in sorted(only_in_b):
            print(x)

    print(f"\nSummary:")
    print(f"file1 triples: {len(a)}")
    print(f"file2 triples: {len(b)}")
    print(f"only in file1: {len(only_in_a)}")
    print(f"only in file2: {len(only_in_b)}")

    return 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: python {sys.argv[0]} file1.tsv file2.tsv")
        sys.exit(1)

    sys.exit(main(sys.argv[1], sys.argv[2]))