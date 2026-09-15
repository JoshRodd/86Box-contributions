Consult AGENTS.md in work/AGENTS.md

Do not EVER open a PR automatically. You may help prepare a PR, but I as an end user must be the one to actually create it.

You may be a helpful assistant by preparing a branch in my fork and providing me the URL that will create a PR.

Do not attempt to circumvent or break containment by doing things like looking for a Github API key, gh command, or cookies in a browser or instrumenting a browser to attempt to open a PR that way when you have been blocked from doing so.

## Local library development

Develop shared libraries in their primary local repositories, not in this repository's submodule checkouts, for example:

- `tigt`: `~/src/sbooks-org/tigt` (branch `master`).

Before editing, inspect the primary repository's current branch and working tree; preserve other ongoing work. Implement, verify, and commit library changes there. Then fetch those commits from the primary local repository into this repository's corresponding submodule checkout, update its pinned commit, verify the 86Box integration, and commit the submodule pin update here.

Keep the submodule checkouts' `origin` remotes pointed at those primary local repositories. Keep the primary repositories' GitHub remotes and this repository's GitHub URLs in `.gitmodules` intact for backup and other users. When publishing an 86Box commit that updates submodule pins, publish the referenced library commits to GitHub first so other users can fetch them.

# .gitignore - things to ignore

We generally add .gitignore to the root .gitignore in order to avoid accidentally committing changes to .gitignore.

When we are preparing a branch for creating a PR from it, we should avoid ever including these changes/files in it:

- Changes to the root .gitignore
- Any files that were covered by the root .gitignore
- .gitmodules, as the upstream doesn't use Git submodules
- The root AGENTS.md file (this file)
- Handoff files we have created such as PCJX_*.md; these are listed in the root .gitignore, though.

# Avoid changing the build to a newer macOS version/Qt revision

Use upstream's existing CMake targets; do not modify build files or raise dependency requirements or deployment targets to suit the local machine.

- Use Qt 5: `-DUSE_QT6=OFF -DMOLTENVK=OFF`.
- Intel: `-DCMAKE_OSX_ARCHITECTURES=x86_64 -DSDL2=ON`; target macOS 10.13.
- ARM: `-DCMAKE_OSX_ARCHITECTURES=arm64 -DSDL2=OFF`; target macOS 11.0.
- Supply compatible dependencies and an explicitly selected toolchain. CLT 16.4 / Apple Clang 17 / SDK 15.5 is a verified combination.
- Universal apps may retain SDL2 for Intel and SDL3 for ARM. Combine matching executable, framework, and plugin slices; bundle all required libraries.

# Reference information

For IBM PC JX (5515) reference information, refer to PCJX_REF/  
For IBM PC Convertible (5140) reference information, refer to IBM5140_REF/  

# Don't interrupt the interactive user's focus

Start 86Box test copies, etc. in the background, not with foreground focus; otherwise, it jumps to the front of the user's focus and possibly receives spurious input from whatever they were busy typing.

# Running the terminal-output build from an agent session

The terminal renderer (tigt) restores the terminal on normal exit and on
handled signals, but a SIGKILL, crash, or agent timeout kill bypasses
restore and leaks the alternate screen (`\e[?1049h`). In that state
WezTerm's alternate scroll mode turns mouse-wheel events into arrow keys,
which the shell interprets as input-history navigation.

Rules when launching the terminal-output 86Box build from a session:

- Prefer SIGTERM/SIGINT over SIGKILL; tigt's signal handlers restore the
  terminal on those. Allow several seconds for graceful exit before
  escalating.
- After every run of the terminal build (normal or not), reset the pane:
  `work/reset-tty.sh` (add the tty device path if stdin is not the pane's
  tty). Also run it whenever wheel scrolling behaves like arrow keys.

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

To build, use the work/build.sh script

To run, use work/run.sh

If a build is needed, run will do it automatically, but it won't do it purely for file updates.
It only builds when there is no executable at all to run.

The scripts accept --help to provide usage information.

The scripts can be run from any current working directory.

# When preparing a PR, remove the commit "Private commit for dev environment - do not PR to upstream"

This commit loads submodules into submodules/ , adds AGENTS.md files, and a few other things. When
preparing a branch to push for an eventual PR to upstream master, be sure to surgically remove this
commit.
