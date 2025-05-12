#!/usr/bin/env python2

#############################################
#                                           #
# Anki_Github                               #
# Library for Anki Specific Github funcions #
#                                           #
#############################################

from github import Github, GithubException
import subprocess
import dependencies
import sys
import os


DEFAULT_REPO = "anki/cozmo-one"

def get_branch_name(auth_token, pull_request_number, repo_name="anki/cozmo-one"):
    """
    Returns the original developer branch name given a github PR number.

    :param auth_token: string
    :param pull_request_number: int
    :param repo_name: string
    :return: string
    """

    # TO-DO: initialize of instance should become its own function if this grows larger.
    try:
        github_inst = Github(auth_token)
        repo = github_inst.get_repo(repo_name)
        for pr in repo.get_pulls():
            if pr.number == pull_request_number:
                return (pr.raw_data['head']['ref'])
    except GithubException as e:
        print("Exception thrown because of {0}".format(repo))
        sys.exit("GITHUB ERROR: {0}".format(e.data['message']))


def get_file_change_list(auth_token, pull_request_number, verbose=False, repo_name=DEFAULT_REPO):
    """
    Returns the list of changed files from a github PR.

    :param auth_token: string
    :param pull_request_number: int
    :param verbose: boolean
    :param repo_name: string
    :return: list
    """

    if dependencies.is_tool("git") is False:
        sys.exit("[ERROR] Could not find git")

    try:
        github_inst = Github(auth_token)
        repo = github_inst.get_repo(repo_name)
        pr = repo.get_pull(pull_request_number)
    except GithubException as e:
        sys.exit("GITHUB ERROR: {0} {1}".format(e.data['message'], e.status))

    if verbose:
        print("Comparing {0} at {1}".format(pr.base.ref, pr.base.sha))
        print("against {0} at {1}".format(pr.head.ref, pr.head.sha))

    git_cmd = ['git', 'diff', '--name-only', '{0}...{1}'.format(pr.base.sha, pr.head.sha), '--submodule']
    p = subprocess.Popen(git_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (stdout, stderr) = p.communicate()
    status = p.poll()

    if status != 0:
        print("ERROR: {0}".format(stderr))
        return None
    if verbose:
        print(os.linesep + "Files Changed:" + os.linesep)
        print(stdout)
        return stdout.splitlines()


def pull_request_merge_info(auth_token, pull_request_number, retries=5, verbose=False, repo_name=DEFAULT_REPO):
    """
    Returns a Dictionary of if it's mergable, and the merged sha to build against.

    :param auth_token: string
    :param pull_request_number: int
    :param verbose: boolean
    :param repo_name: string
    :return: Dictionary
    """

    results = {}
    if dependencies.is_tool("git") is False:
        sys.exit("[ERROR] Could not find git")
    for x in range(retries):
        try:
            github_inst = Github(auth_token)
            repo = github_inst.get_repo(repo_name)
            pr = repo.get_pull(pull_request_number)
            results = {"mergeable": pr.mergeable, "merge_commit_sha": pr.merge_commit_sha}
            if pr.mergeable:  # Not sure why some times it takes a few tries.
                break
        except GithubException as e:
            sys.exit("GITHUB ERROR: {0} {1}".format(e.data['message'], e.status))

    if verbose:
        print("Comparing {0} at {1}".format(pr.base.ref, pr.base.sha))
        print("against {0} at {1}".format(pr.head.ref, pr.head.sha))

    return results

def post_ci_bot_comment(auth_token, pull_request_number, mrkdwn_comment, repo_name="anki/victor"):
    """
    Returns a :class:`github.CommitComment.CommitComment`

    :param auth_token: string
    :param pull_request_number: int
    :param mrkdwn_comment: string
    :param repo_name: string
    :return: :class:`github.CommitComment.CommitComment`
    """
    try:
        gh = Github(auth_token)
        head_commit = get_pr_head_commit(auth_token, pull_request_number, repo_name)
        rendered_markdown_comment = gh.render_markdown(mrkdwn_comment)
        comment = head_commit.create_comment(rendered_markdown_comment)
        return comment
        
    except GithubException as e:
        print("Exception thrown because of {0}".format(e))
        sys.exit("GITHUB ERROR: {0}".format(e.data['message']))

def get_pr_head_commit(auth_token, pull_request_number, repo_name="anki/victor"):
    """
    Returns a :class:`github.CommitComment.CommitComment`

    :param auth_token: string
    :param pull_request_number: int
    :param mrkdwn_comment: string
    :param repo_name: string
    :return: :class:`github.Commit.Commit`
    """
    try:
        gh = Github(auth_token)
        repo = gh.get_repo(repo_name)
        pull = repo.get_pull(pull_request_number)
        head_commit_sha = repo.get_commit(pull.head.sha)
        return head_commit_sha
        
    except GithubException as e:
        print("Exception thrown because of {0}".format(e))
        sys.exit("GITHUB ERROR: {0}".format(e.data['message']))


def post_pending_commit_status(auth_token, pull_request_number, build_url, 
    build_ctx, description_msg, repo_name="anki/victor"):
    """
    Returns a :class:`github.CommitComment.CommitComment`

    :param auth_token: string
    :param pull_request_number: int
    :param build_url: string
    :param build_ctx: string
    :param description_msg: string
    :param repo_name: string
    :return: :class:`github.CommitStatus.CommitStatus`
    """
    try:
        head_commit = get_pr_head_commit(auth_token, pull_request_number)
        status = head_commit.create_status(
            state="pending",
            target_url=build_url,
            description=description_msg,
            context=build_ctx,
        )
        return status
        
    except GithubException as e:
        print("Exception thrown because of {0}".format(e))
        sys.exit("GITHUB ERROR: {0}".format(e.data['message']))
    