#!/bin/zsh

repo_name=86Box
if [[ $# -le 0 ]]; then printf "First argument must be a filename.\n" >&2; exit 1; fi; filename=$1; shift
if [[ $# -le 0 ]]; then printf "Second argument must be a branch name.\n" >&2; exit 1; fi; branch=$1; shift

echo git fetch origin master:master
echo git add $filename
echo git commit --only $filename
local_branch=pr/$branch
worktree_dir=../"$repo_name"-pr-"$branch"
echo git worktree add -b $local_branch $worktree_dir master
echo git -C $worktree_dir cherry-pick \$\(git rev-parse HEAD\)
echo git -C $worktree_dir push -u fork $local_branch:$branch
echo git worktree remove $worktree_dir
echo git branch -d $local_branch
