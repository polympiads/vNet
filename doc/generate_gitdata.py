
import importlib
import os
import subprocess

spec = importlib.util.spec_from_file_location( "listbranches", os.path.join(os.path.dirname( __file__ ), "listbranches.py") )
listbranches = importlib.util.module_from_spec(spec)
spec.loader.exec_module(listbranches)

get_all_branches = listbranches.get_all_branches

def run_command (cmd: str):
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    stdout, stderr = process.communicate()
    return stdout.strip().decode("utf-8")
def generate_gitdata (version_root):
    git_branch   = run_command("git rev-parse --abbrev-ref HEAD".split())
    all_branches = get_all_branches()

    return {
        "git_branch": git_branch.strip(),
        "all_branches": [
            {
                "name": branch.strip(),
                "path": version_root if branch == "release" else version_root + branch
            }
            for branch in all_branches if branch != "gh-pages"
        ]
    }
