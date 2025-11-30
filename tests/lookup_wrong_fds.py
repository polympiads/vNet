
import sys
import os

print()
print()
print("Search for wrong file descriptors")
folder = sys.argv[1]

print(" - search folder :", folder)

files_to_search = []

for file in os.listdir(folder):
    words = file.split(".")
    if len(words) == 3 and words[0] == "MemoryChecker" and words[2] == "log":
        files_to_search.append((int(words[1]), os.path.join(folder, file)))

print()

files_to_search.sort()

should_exit = False
for uuid, file in  files_to_search:
    with open(file, "r") as tfile:
        lines = tfile.read().split("\n")
    
    lines.append("")
    invalid = 0
    for idx in range( len(lines) - 1 ):
        if "Open" in lines[idx] and "<inherited" not in lines[idx + 1]:
            invalid += 1

    if invalid != 0:
        should_exit = True
        print(f"Test {uuid} has FD leaks :")
        print(" - Number of FDs still open :", invalid)
        print()

if should_exit:
    print("ERROR: Some file descriptors haven't been closed.")
    exit(1)
else:
    print("No wrong file descriptors found.")
    print()