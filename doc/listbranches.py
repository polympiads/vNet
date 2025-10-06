
import subprocess

def get_all_branches ():
    result = subprocess.run( [ "git", "branch", "-r" ], capture_output=True )
    assert result.returncode == 0
    out = result.stdout.decode()
    lines = out.splitlines()

    result = []
    for line in lines:
        line = line.strip()
        if not line.startswith("origin/"):
            continue
        line = line[7:]
        if "/" in line: continue
        if " " in line: continue
        result.append(line)
    
    return result

def main ():
    result = get_all_branches()

    for line in result:
        print(line)

if __name__ == "__main__":
    main()
