Consult AGENTS.md in work/AGENTS.md

Do not EVER open a PR automatically. You may help prepare a PR, but I as an end user must be the one to actually create it.

You may be a helpful assistant by preparing a branch in my fork and providing me the URL that will create a PR.

Do not attempt to circumvent or break containment by doing things like looking for a Github API key, gh command, or cookies in a browser or instrumenting a browser to attempt to open a PR that way when you have been blocked from doing so.

## Local library development

Develop shared libraries in this repository's submodule checkouts, for example:

- `tigt`: `submodules/tigt` (branch `master`).

# .gitignore - things to ignore

When we are preparing a branch for creating a PR from it, we should avoid ever including these changes/files in it:

- Changes to the root .gitignore
- Anything in work/
- .gitmodules, as the upstream doesn't use Git submodules
- The root AGENTS.md file

# Avoid changing the build to a newer macOS version/Qt revision

Use upstream's existing CMake targets; do not modify build files or raise dependency requirements or deployment targets to suit the local machine.

Use the work/build.sh utility to build. Use work/run.sh the resultant executable. Under most circumstances, using the --terminal option on build will avoid needless building and linking Qt and using --tty on run will make startup time faster.

Use the gdb builds if you need them; use `work/run.sh --tty --gdb`, or `work/run.sh --gdb` if for some reason you need to run the GUI version.

# Reference information

For IBM 7690 Clinical Workstation reference information, refer to submodules/IBM7690_REF/  
For general IBM PS/2 Model 25 / 30 information, refer to ~/src/bios8530

# Use the 7690 machine for testing

Use the machine named 7690 for testing.

If you need a second copy, or the one given to you becomes corrupted or is missing, copy the template from work/7690/ for a fresh machine.

This machine is configured with Drive C: with DOS 3.20 and the 7690 diagnostic files in C:\DIAGS

mtools is configured that its drive C: will map to the 7690 machine's drive C: so you can easily transfer files using mcopy, mdir, etc.

C:\POWEROFF.COM can be used to gracefully shut down the machine.

# Don't interrupt the interactive user's focus

If you start a GUI version of 86Box, don't steal the foreground focus. Use the -g option to work/run.sh or else use open -g if opening the 86Box binary directly.

# Work directories

When doing work, prefer placing files that really do not need to end up in
Git long term in work/

work/tmp/ is intended for temporary files.

work/fix/ is intended for files to support fixing a particular issue associated
with a branch. So if there is a branch called fix/issue4567-fdc-broken, we would
make a directory called work/fix/issue4567-fdc-broken/ that would contain files
to help assist with that work.

# Avoid merge commits when pulling upstream

Avoid doing merge commits to pull from upstream. Instead, rebase on top of the
commits from upstream with whatever our changes are.

# How to build and run

To [build](build), use the work/build.sh script

To run, use work/run.sh

If a build is needed, run will do it automatically, but it won't do it purely for file updates.
It only builds when there is no executable at all to run.

The scripts accept --help to provide usage information.

The scripts can be run from any current working directory.

# When preparing a PR, remove the commit "Private commit for dev environment - do not PR to upstream"

This commit loads submodules into submodules/ , adds AGENTS.md files, and a few other things. When
preparing a branch to push for an eventual PR to upstream master, be sure to surgically remove this
commit. In essence, make a new branch based off of upsteam master HEAD and cherry pick the commits
from your active work branch into that that you want in the PR.
