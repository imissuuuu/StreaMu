#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib import error, parse, request

JsonObject = dict[str, Any]
STATE_COMMENT_MARKER = "streamu-automation-state"
REVIEW_COMMENT_MARKER = "streamu-review-result"
PHASE_LABELS: tuple[str, ...] = (
    "ai-review-running",
    "needs-decision",
    "needs-maintainer-fix",
    "review-approved",
    "release-draft-ready",
)
PERSISTENT_LABELS: tuple[str, ...] = (
    "device-test-passed",
    "release-requested",
)
DEVICE_TEST_CHECKBOX = "- [x] Device test passed"
RELEASE_REQUESTED_CHECKBOX = "- [x] Create draft release after review passes"
REVIEW_ONLY_PATH_PREFIXES: tuple[str, ...] = ("docs/", ".github/", "tools/", ".takt/")
CPP_PATH_PREFIXES: tuple[str, ...] = ("source/", "include/")
PYTHON_PATH_PREFIXES: tuple[str, ...] = ("server/", "tools/", ".github/scripts/")
CPP_FILE_EXTENSIONS: tuple[str, ...] = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp")
PYTHON_FILE_EXTENSIONS: tuple[str, ...] = (".py",)
PUBLISHABLE_PATH_PREFIXES: tuple[str, ...] = (
    ".github/",
    "android/",
    "assets/",
    "include/",
    "scripts/",
    "server/",
    "source/",
    "third_party/",
)
PUBLISHABLE_EXACT_PATHS: tuple[str, ...] = (
    ".gitignore",
    "LICENSE",
    "Makefile",
    "README.md",
    "THIRD_PARTY_LICENSES.md",
    "app.rsf",
    "banner.bnr",
    "build.sh",
)
LOCAL_ONLY_PATH_PREFIXES: tuple[str, ...] = (
    ".claude/",
    ".mypy_cache/",
    ".ruff_cache/",
    ".takt/",
    "docs/",
    "dumps/",
    "HANDOFF/",
    "logs/",
    "mockup/",
    "release-artifacts-",
    "test_wallpapers/",
    "tools/",
    "venv/",
    "__pycache__/",
)
LOCAL_ONLY_EXACT_PATHS: tuple[str, ...] = (
    ".clang-format",
    "pyproject.toml",
    "review_result.json",
    "startup_perf.log",
)
RELEASE_NOTES_PATTERN = re.compile(r"^RELEASE_NOTES_v[0-9]+\.[0-9]+\.[0-9]+\.md$")


@dataclass(frozen=True)
class PullRequestContext:
    number: int
    head_ref: str
    head_sha: str
    base_ref: str
    base_sha: str
    labels: tuple[str, ...]
    body: str
    changed_files: tuple[str, ...]


@dataclass(frozen=True)
class ReviewSelection:
    workflow_name: str
    touches_python: bool
    touches_cpp: bool
    touches_ci_only: bool
    release_requested: bool
    release_version: str | None


@dataclass(frozen=True)
class AutomationState:
    phase: str
    autofix_attempts: int
    review_attempts: int
    selected_workflow: str | None
    release_requested: bool
    release_version: str | None


@dataclass(frozen=True)
class ReviewDecision:
    decision: str
    auto_fix_allowed: bool
    summary: str
    user_decision_needed: bool
    user_prompt: str | None


@dataclass(frozen=True)
class StaticCheckPlan:
    ruff_targets: tuple[str, ...]
    py_compile_targets: tuple[str, ...]
    mypy_targets: tuple[str, ...]


@dataclass(frozen=True)
class LocalInspection:
    branch: str
    changed_files: tuple[str, ...]
    selected_workflow: str
    touches_python: bool
    touches_cpp: bool


@dataclass(frozen=True)
class ReleasePlan:
    version: str
    tag_name: str
    release_title: str
    release_notes_path: str
    asset_paths: tuple[str, ...]
    target_sha: str


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="GitHub automation helper for StreaMu PR CI.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect-pr")
    inspect_parser.add_argument("--event-path", type=Path, required=True)
    inspect_parser.add_argument("--repo-root", type=Path, required=True)
    inspect_parser.add_argument("--write-github-output", type=Path)

    inspect_local_parser = subparsers.add_parser("inspect-local")
    inspect_local_parser.add_argument("--repo-root", type=Path, required=True)
    inspect_local_parser.add_argument("--scope-path", action="append", default=[])

    autofix_parser = subparsers.add_parser("apply-deterministic-autofix")
    autofix_parser.add_argument("--repo-root", type=Path, required=True)
    autofix_parser.add_argument("--changed-files-json", required=True)
    autofix_parser.add_argument("--write-github-output", type=Path)

    local_checks_parser = subparsers.add_parser("run-local-checks")
    local_checks_parser.add_argument("--repo-root", type=Path, required=True)
    local_checks_parser.add_argument("--skip-build", action="store_true")
    local_checks_parser.add_argument("--scope-path", action="append", default=[])

    publishable_parser = subparsers.add_parser("classify-publishable")
    publishable_parser.add_argument("--paths-json", required=True)

    static_checks_parser = subparsers.add_parser("run-static-checks")
    static_checks_parser.add_argument("--repo-root", type=Path, required=True)
    static_checks_parser.add_argument("--ruff-targets-json", required=True)
    static_checks_parser.add_argument("--py-compile-targets-json", required=True)
    static_checks_parser.add_argument("--mypy-targets-json", required=True)

    sync_parser = subparsers.add_parser("sync-pr-state")
    sync_parser.add_argument("--event-path", type=Path, required=True)
    sync_parser.add_argument("--phase", required=True)
    sync_parser.add_argument("--selected-workflow")
    sync_parser.add_argument("--device-test-passed", choices=("true", "false"), required=True)
    sync_parser.add_argument("--release-requested", choices=("true", "false"), required=True)
    sync_parser.add_argument("--release-version")
    sync_parser.add_argument("--increment-autofix-attempts", action="store_true")
    sync_parser.add_argument("--increment-review-attempts", action="store_true")

    read_state_parser = subparsers.add_parser("read-pr-state")
    read_state_parser.add_argument("--event-path", type=Path, required=True)
    read_state_parser.add_argument("--write-github-output", type=Path)

    parse_report_parser = subparsers.add_parser("parse-review-report")
    parse_report_parser.add_argument("--runs-root", type=Path)
    parse_report_parser.add_argument("--report-path", type=Path)
    parse_report_parser.add_argument("--write-github-output", type=Path)

    publish_review_parser = subparsers.add_parser("publish-review-comment")
    publish_review_parser.add_argument("--event-path", type=Path, required=True)
    publish_review_parser.add_argument("--decision", required=True)
    publish_review_parser.add_argument("--summary", required=True)
    publish_review_parser.add_argument(
        "--user-decision-needed", choices=("true", "false"), required=True
    )
    publish_review_parser.add_argument("--user-prompt")

    autofix_task_parser = subparsers.add_parser("build-autofix-task")
    autofix_task_parser.add_argument("--report-path", type=Path, required=True)
    autofix_task_parser.add_argument("--output-path", type=Path, required=True)
    autofix_task_parser.add_argument("--pr-number", type=int, required=True)
    autofix_task_parser.add_argument("--base-sha", required=True)

    release_plan_parser = subparsers.add_parser("prepare-release-local")
    release_plan_parser.add_argument("--repo-root", type=Path, required=True)

    return parser.parse_args(argv)


def load_json_file(path: Path) -> JsonObject:
    with path.open("r", encoding="utf-8-sig") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError(f"expected object at {path}")
    return data


def get_required_dict(data: JsonObject, key: str) -> JsonObject:
    value = data.get(key)
    if not isinstance(value, dict):
        raise ValueError(f"missing object field: {key}")
    return value


def get_required_str(data: JsonObject, key: str) -> str:
    value = data.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError(f"missing string field: {key}")
    return value


def get_optional_str(data: JsonObject, key: str) -> str | None:
    value = data.get(key)
    return value if isinstance(value, str) and value else None


def parse_bool_string(value: str) -> bool:
    return value == "true"


def write_github_output(output_path: Path | None, values: dict[str, str]) -> None:
    if output_path is None:
        return
    with output_path.open("a", encoding="utf-8", newline="\n") as handle:
        for key, value in values.items():
            handle.write(f"{key}={value}\n")


def run_git(repo_root: Path, args: list[str]) -> subprocess.CompletedProcess[str]:
    command = ["git", "-C", str(repo_root)] + args
    return subprocess.run(command, check=True, capture_output=True, text=True)


def collect_changed_files(repo_root: Path, base_sha: str, head_sha: str) -> tuple[str, ...]:
    result = run_git(repo_root, ["diff", "--name-only", f"{base_sha}...{head_sha}"])
    paths = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return tuple(paths)


def parse_git_name_list(stdout: str) -> tuple[str, ...]:
    seen: set[str] = set()
    normalized_paths: list[str] = []
    for raw_line in stdout.splitlines():
        path = raw_line.strip().replace("\\", "/")
        if not path or path in seen:
            continue
        seen.add(path)
        normalized_paths.append(path)
    return tuple(normalized_paths)


def collect_worktree_changed_files(repo_root: Path) -> tuple[str, ...]:
    seen: set[str] = set()
    changed_files: list[str] = []
    command_sets = (
        ["diff", "--name-only", "--"],
        ["diff", "--cached", "--name-only", "--"],
        ["ls-files", "--others", "--exclude-standard"],
    )

    for args in command_sets:
        result = run_git(repo_root, args)
        for path in parse_git_name_list(result.stdout):
            if path in seen:
                continue
            seen.add(path)
            changed_files.append(path)
    return tuple(changed_files)


def get_current_branch(repo_root: Path) -> str:
    result = run_git(repo_root, ["rev-parse", "--abbrev-ref", "HEAD"])
    branch = result.stdout.strip()
    if not branch:
        raise ValueError("current branch could not be determined")
    return branch


def read_head_sha(repo_root: Path) -> str:
    result = run_git(repo_root, ["rev-parse", "HEAD"])
    head_sha = result.stdout.strip()
    if not head_sha:
        raise ValueError("HEAD SHA could not be determined")
    return head_sha


def normalize_scope_paths(scope_paths: list[str]) -> tuple[str, ...]:
    normalized: list[str] = []
    seen: set[str] = set()
    for path in scope_paths:
        candidate = path.replace("\\", "/").strip().strip("/")
        if not candidate or candidate in seen:
            continue
        seen.add(candidate)
        normalized.append(candidate)
    return tuple(normalized)


def filter_changed_files_by_scope(
    changed_files: tuple[str, ...], scope_paths: tuple[str, ...]
) -> tuple[str, ...]:
    if not scope_paths:
        return changed_files

    filtered: list[str] = []
    for changed_file in changed_files:
        normalized_file = changed_file.replace("\\", "/")
        for scope_path in scope_paths:
            if normalized_file == scope_path or normalized_file.startswith(f"{scope_path}/"):
                filtered.append(changed_file)
                break
    return tuple(filtered)


def is_publishable_path(path: str) -> bool:
    normalized_path = path.replace("\\", "/").strip().strip("/")
    if not normalized_path:
        return False
    if normalized_path in LOCAL_ONLY_EXACT_PATHS:
        return False
    if any(
        normalized_path == prefix.rstrip("/") or normalized_path.startswith(prefix)
        for prefix in LOCAL_ONLY_PATH_PREFIXES
    ):
        return False
    if normalized_path in PUBLISHABLE_EXACT_PATHS:
        return True
    if RELEASE_NOTES_PATTERN.fullmatch(normalized_path):
        return True
    return any(
        normalized_path == prefix.rstrip("/") or normalized_path.startswith(prefix)
        for prefix in PUBLISHABLE_PATH_PREFIXES
    )


def split_publishable_paths(
    changed_files: tuple[str, ...],
) -> tuple[tuple[str, ...], tuple[str, ...]]:
    publishable_paths: list[str] = []
    blocked_paths: list[str] = []
    for path in changed_files:
        if is_publishable_path(path):
            publishable_paths.append(path)
        else:
            blocked_paths.append(path)
    return tuple(publishable_paths), tuple(blocked_paths)


def load_pull_request_context(event_path: Path, repo_root: Path) -> PullRequestContext:
    event = load_json_file(event_path)
    pull_request = get_required_dict(event, "pull_request")
    head = get_required_dict(pull_request, "head")
    base = get_required_dict(pull_request, "base")

    labels_raw = pull_request.get("labels")
    labels: list[str] = []
    if isinstance(labels_raw, list):
        for label in labels_raw:
            if isinstance(label, dict):
                name = get_optional_str(label, "name")
                if name is not None:
                    labels.append(name)

    body = get_optional_str(pull_request, "body") or ""
    changed_files = collect_changed_files(
        repo_root=repo_root,
        base_sha=get_required_str(base, "sha"),
        head_sha=get_required_str(head, "sha"),
    )

    pr_number = event.get("number")
    if not isinstance(pr_number, int):
        pr_number = pull_request.get("number")
    if not isinstance(pr_number, int):
        raise ValueError("pull request number missing from event payload")

    return PullRequestContext(
        number=pr_number,
        head_ref=get_required_str(head, "ref"),
        head_sha=get_required_str(head, "sha"),
        base_ref=get_required_str(base, "ref"),
        base_sha=get_required_str(base, "sha"),
        labels=tuple(labels),
        body=body,
        changed_files=changed_files,
    )


def detect_release_requested(body: str) -> bool:
    return RELEASE_REQUESTED_CHECKBOX in body


def detect_device_test_passed(body: str) -> bool:
    return DEVICE_TEST_CHECKBOX in body


def read_release_version(repo_root: Path) -> str | None:
    makefile_path = repo_root / "Makefile"
    if not makefile_path.is_file():
        return None

    pattern = re.compile(r"^VERSION\s*:=\s*([0-9]+\.[0-9]+\.[0-9]+)\s*$")
    with makefile_path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            match = pattern.match(raw_line.strip())
            if match is not None:
                return match.group(1)
    return None


def release_notes_exists(repo_root: Path, version: str | None) -> bool:
    if version is None:
        return False
    return (repo_root / f"RELEASE_NOTES_v{version}.md").is_file()


def select_review_workflow(
    changed_files: tuple[str, ...], release_requested: bool, release_version: str | None
) -> ReviewSelection:
    touches_python = any(
        path.startswith(PYTHON_PATH_PREFIXES) and path.endswith(PYTHON_FILE_EXTENSIONS)
        for path in changed_files
    )
    touches_cpp = any(
        path.startswith(CPP_PATH_PREFIXES) and path.endswith(CPP_FILE_EXTENSIONS)
        for path in changed_files
    )
    touches_ci_only = bool(changed_files) and all(
        any(path.startswith(prefix) for prefix in REVIEW_ONLY_PATH_PREFIXES)
        for path in changed_files
    )

    workflow_name = "review-only-ci"
    if touches_python or touches_cpp:
        workflow_name = "review-pipeline-ci"

    return ReviewSelection(
        workflow_name=workflow_name,
        touches_python=touches_python,
        touches_cpp=touches_cpp,
        touches_ci_only=touches_ci_only,
        release_requested=release_requested,
        release_version=release_version,
    )


def select_local_review_workflow(changed_files: tuple[str, ...]) -> LocalInspection:
    selection = select_review_workflow(
        changed_files=changed_files,
        release_requested=False,
        release_version=None,
    )
    local_workflow = (
        "review-pipeline" if selection.workflow_name == "review-pipeline-ci" else "review-only"
    )
    return LocalInspection(
        branch="",
        changed_files=changed_files,
        selected_workflow=local_workflow,
        touches_python=selection.touches_python,
        touches_cpp=selection.touches_cpp,
    )


def build_static_check_plan(changed_files: tuple[str, ...]) -> StaticCheckPlan:
    ruff_targets: list[str] = []
    py_compile_targets: list[str] = []
    mypy_targets: list[str] = []
    seen_ruff: set[str] = set()
    seen_py_compile: set[str] = set()
    seen_mypy: set[str] = set()

    for path in changed_files:
        suffix = Path(path).suffix.lower()
        if suffix not in PYTHON_FILE_EXTENSIONS:
            continue
        if not path.startswith(PYTHON_PATH_PREFIXES):
            continue
        if path not in seen_ruff:
            ruff_targets.append(path)
            seen_ruff.add(path)
        if path not in seen_py_compile:
            py_compile_targets.append(path)
            seen_py_compile.add(path)
        if path.startswith(".github/scripts/") and path not in seen_mypy:
            mypy_targets.append(path)
            seen_mypy.add(path)

    return StaticCheckPlan(
        ruff_targets=tuple(ruff_targets),
        py_compile_targets=tuple(py_compile_targets),
        mypy_targets=tuple(mypy_targets),
    )


def print_json(payload: JsonObject) -> None:
    print(json.dumps(payload, ensure_ascii=True, indent=2))


def handle_inspect_pr(event_path: Path, repo_root: Path, output_path: Path | None) -> int:
    context = load_pull_request_context(event_path=event_path, repo_root=repo_root)
    device_test_passed = detect_device_test_passed(context.body)
    release_requested = detect_release_requested(context.body)
    release_version = read_release_version(repo_root)
    release_eligible = release_requested and release_notes_exists(repo_root, release_version)
    selection = select_review_workflow(
        changed_files=context.changed_files,
        release_requested=release_requested,
        release_version=release_version,
    )
    static_check_plan = build_static_check_plan(context.changed_files)

    outputs = {
        "changed_files_json": json.dumps(list(context.changed_files), ensure_ascii=True),
        "device_test_passed": "true" if device_test_passed else "false",
        "release_requested": "true" if release_requested else "false",
        "selected_review_workflow": selection.workflow_name,
        "release_version": release_version or "",
        "release_eligible": "true" if release_eligible else "false",
        "ruff_targets_json": json.dumps(list(static_check_plan.ruff_targets), ensure_ascii=True),
        "py_compile_targets_json": json.dumps(
            list(static_check_plan.py_compile_targets), ensure_ascii=True
        ),
        "mypy_targets_json": json.dumps(list(static_check_plan.mypy_targets), ensure_ascii=True),
    }
    write_github_output(output_path, outputs)

    print_json(
        {
            "pull_request": context.number,
            "device_test_passed": device_test_passed,
            "release_requested": release_requested,
            "selected_review_workflow": selection.workflow_name,
            "release_version": release_version,
            "release_eligible": release_eligible,
            "changed_files": list(context.changed_files),
            "ruff_targets": list(static_check_plan.ruff_targets),
            "py_compile_targets": list(static_check_plan.py_compile_targets),
            "mypy_targets": list(static_check_plan.mypy_targets),
        }
    )
    return 0


def handle_inspect_local(repo_root: Path, scope_paths: tuple[str, ...]) -> int:
    scoped_changed_files = filter_changed_files_by_scope(
        collect_worktree_changed_files(repo_root),
        scope_paths,
    )
    publishable_changed_files, blocked_paths = split_publishable_paths(scoped_changed_files)
    inspection = select_local_review_workflow(publishable_changed_files)
    payload: JsonObject = {
        "branch": get_current_branch(repo_root),
        "changed_files": list(inspection.changed_files),
        "blocked_paths": list(blocked_paths),
        "selected_workflow": inspection.selected_workflow,
        "touches_python": inspection.touches_python,
        "touches_cpp": inspection.touches_cpp,
        "release_version": read_release_version(repo_root),
        "scope_paths": list(scope_paths),
    }
    print_json(payload)
    return 0


def split_autofix_targets(
    changed_files: tuple[str, ...],
) -> tuple[list[str], list[str]]:
    python_files: list[str] = []
    cpp_files: list[str] = []
    for path in changed_files:
        suffix = Path(path).suffix.lower()
        if path.startswith(PYTHON_PATH_PREFIXES) and suffix in PYTHON_FILE_EXTENSIONS:
            python_files.append(path)
        if path.startswith(CPP_PATH_PREFIXES) and suffix in CPP_FILE_EXTENSIONS:
            cpp_files.append(path)
    return python_files, cpp_files


def run_command(repo_root: Path, args: list[str]) -> None:
    subprocess.run(args, cwd=repo_root, check=True)


def filter_existing_files(repo_root: Path, changed_files: tuple[str, ...]) -> tuple[str, ...]:
    existing_files: list[str] = []
    for path in changed_files:
        if (repo_root / path).is_file():
            existing_files.append(path)
    return tuple(existing_files)


def apply_deterministic_autofix_to_files(
    repo_root: Path, changed_files: tuple[str, ...]
) -> tuple[tuple[str, ...], list[str], list[str]]:
    existing_files = filter_existing_files(repo_root, changed_files)
    python_files, cpp_files = split_autofix_targets(existing_files)

    if python_files:
        run_command(repo_root, ["ruff", "check", "--fix", *python_files])
        run_command(repo_root, ["ruff", "format", *python_files])
    if cpp_files:
        run_command(repo_root, ["clang-format", "-i", *cpp_files])

    changed_after_fix = collect_worktree_changed_files(repo_root)
    return changed_after_fix, python_files, cpp_files


def run_static_check_plan(repo_root: Path, static_check_plan: StaticCheckPlan) -> None:
    if static_check_plan.ruff_targets:
        run_command(repo_root, ["ruff", "check", *static_check_plan.ruff_targets])
    if static_check_plan.py_compile_targets:
        run_command(
            repo_root,
            [sys.executable, "-m", "py_compile", *static_check_plan.py_compile_targets],
        )
    if static_check_plan.mypy_targets:
        run_command(repo_root, ["mypy", *static_check_plan.mypy_targets])


def run_local_3ds_build(repo_root: Path) -> None:
    jobs = str(max(1, os.cpu_count() or 1))
    run_command(repo_root, ["make", "clean"])
    run_command(repo_root, ["make", f"-j{jobs}"])


def run_local_server_build(repo_root: Path) -> None:
    server_root = repo_root / "server"
    subprocess.run(
        ["cmd", "/c", "build_exe.bat", "--ci"],
        cwd=server_root,
        check=True,
    )


def run_local_cppcheck(repo_root: Path, changed_files: tuple[str, ...]) -> None:
    cpp_targets = [
        path
        for path in changed_files
        if path.startswith(CPP_PATH_PREFIXES) and Path(path).suffix.lower() in CPP_FILE_EXTENSIONS
    ]
    if not cpp_targets:
        return
    run_command(
        repo_root,
        [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(repo_root / "scripts" / "run_cppcheck.ps1"),
            *cpp_targets,
        ],
    )


def collect_git_diff_paths(repo_root: Path) -> tuple[str, ...]:
    return collect_worktree_changed_files(repo_root)


def parse_existing_state(body: str) -> AutomationState | None:
    if STATE_COMMENT_MARKER not in body:
        return None

    lines = [line.strip() for line in body.splitlines()]
    values: dict[str, str] = {}
    for line in lines:
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value

    phase = values.get("phase")
    if not phase:
        return None

    return AutomationState(
        phase=phase,
        autofix_attempts=int(values.get("autofix_attempts", "0")),
        review_attempts=int(values.get("review_attempts", "0")),
        selected_workflow=values.get("selected_workflow") or None,
        release_requested=values.get("release_requested") == "true",
        release_version=values.get("release_version") or None,
    )


def handle_apply_deterministic_autofix(
    repo_root: Path, changed_files_json: str, output_path: Path | None
) -> int:
    raw_changed_files = json.loads(changed_files_json)
    if not isinstance(raw_changed_files, list) or not all(
        isinstance(path, str) for path in raw_changed_files
    ):
        raise ValueError("changed-files-json must be a JSON array of strings")

    changed_files = tuple(raw_changed_files)
    changed_after_fix, python_files, cpp_files = apply_deterministic_autofix_to_files(
        repo_root, changed_files
    )
    outputs = {
        "changed": "true" if bool(changed_after_fix) else "false",
        "changed_paths_json": json.dumps(list(changed_after_fix), ensure_ascii=True),
    }
    write_github_output(output_path, outputs)
    print_json(
        {
            "changed": bool(changed_after_fix),
            "changed_paths": list(changed_after_fix),
            "python_targets": python_files,
            "cpp_targets": cpp_files,
        }
    )
    return 0


def handle_run_local_checks(repo_root: Path, skip_build: bool, scope_paths: tuple[str, ...]) -> int:
    scoped_changed_files = filter_changed_files_by_scope(
        collect_worktree_changed_files(repo_root),
        scope_paths,
    )
    changed_files, blocked_paths = split_publishable_paths(scoped_changed_files)
    if not changed_files:
        if blocked_paths:
            blocked_display = ", ".join(blocked_paths)
            raise ValueError(
                f"no publishable local changes found; blocked local-only paths: {blocked_display}"
            )
        raise ValueError("no local changes found")

    changed_after_fix, _, _ = apply_deterministic_autofix_to_files(repo_root, changed_files)
    effective_changed_files, blocked_after_fix = split_publishable_paths(
        changed_after_fix or collect_worktree_changed_files(repo_root)
    )
    static_check_plan = build_static_check_plan(effective_changed_files)
    run_static_check_plan(repo_root, static_check_plan)

    touches_cpp = any(
        path.startswith(CPP_PATH_PREFIXES) and Path(path).suffix.lower() in CPP_FILE_EXTENSIONS
        for path in effective_changed_files
    )
    touches_server_python = any(
        path.startswith("server/") and Path(path).suffix.lower() in PYTHON_FILE_EXTENSIONS
        for path in effective_changed_files
    )

    if touches_cpp:
        run_local_cppcheck(repo_root, effective_changed_files)

    ran_3ds_build = False
    ran_server_build = False
    if not skip_build:
        if touches_cpp:
            run_local_3ds_build(repo_root)
            ran_3ds_build = True
        if touches_server_python:
            run_local_server_build(repo_root)
            ran_server_build = True

    print_json(
        {
            "changed_files": list(effective_changed_files),
            "ruff_targets": list(static_check_plan.ruff_targets),
            "py_compile_targets": list(static_check_plan.py_compile_targets),
            "mypy_targets": list(static_check_plan.mypy_targets),
            "ran_3ds_build": ran_3ds_build,
            "ran_server_build": ran_server_build,
            "blocked_paths": list(blocked_after_fix),
            "scope_paths": list(scope_paths),
        }
    )
    return 0


def parse_string_json_array(raw_value: str, argument_name: str) -> tuple[str, ...]:
    parsed = json.loads(raw_value)
    if not isinstance(parsed, list) or not all(isinstance(item, str) for item in parsed):
        raise ValueError(f"{argument_name} must be a JSON array of strings")
    return tuple(parsed)


def handle_classify_publishable(paths_json: str) -> int:
    paths = parse_string_json_array(paths_json, "paths-json")
    publishable_paths, blocked_paths = split_publishable_paths(paths)
    print_json(
        {
            "publishable_paths": list(publishable_paths),
            "blocked_paths": list(blocked_paths),
        }
    )
    return 0


def handle_run_static_checks(
    repo_root: Path,
    ruff_targets_json: str,
    py_compile_targets_json: str,
    mypy_targets_json: str,
) -> int:
    ruff_targets = parse_string_json_array(ruff_targets_json, "--ruff-targets-json")
    py_compile_targets = parse_string_json_array(
        py_compile_targets_json, "--py-compile-targets-json"
    )
    mypy_targets = parse_string_json_array(mypy_targets_json, "--mypy-targets-json")
    run_static_check_plan(
        repo_root,
        StaticCheckPlan(
            ruff_targets=ruff_targets,
            py_compile_targets=py_compile_targets,
            mypy_targets=mypy_targets,
        ),
    )

    print_json(
        {
            "ruff_targets": list(ruff_targets),
            "py_compile_targets": list(py_compile_targets),
            "mypy_targets": list(mypy_targets),
        }
    )
    return 0


def phase_to_labels(phase: str) -> tuple[str, ...]:
    if phase in PHASE_LABELS:
        return (phase,)
    return ()


def build_state_comment(state: AutomationState) -> str:
    selected_workflow = state.selected_workflow or ""
    release_version = state.release_version or ""
    release_requested = "true" if state.release_requested else "false"
    return (
        f"<!-- {STATE_COMMENT_MARKER}\n"
        f"phase={state.phase}\n"
        f"autofix_attempts={state.autofix_attempts}\n"
        f"review_attempts={state.review_attempts}\n"
        f"selected_workflow={selected_workflow}\n"
        f"release_requested={release_requested}\n"
        f"release_version={release_version}\n"
        f"-->"
    )


def build_review_comment(decision: ReviewDecision) -> str:
    action_lines: list[str]
    if decision.decision == "APPROVE":
        action_lines = ["- No blocking review issue remains. Release automation can continue."]
    elif decision.decision == "AUTO_FIXABLE":
        if decision.auto_fix_allowed:
            action_lines = [
                "- Safe mechanical fixes were identified and the automation will attempt them."
            ]
        else:
            action_lines = [
                "- Auto-fix was requested, but the workflow could not safely "
                "proceed without maintainer follow-up."
            ]
    elif decision.decision == "NEEDS_DECISION":
        prompt = (
            decision.user_prompt or "Please reply with the preferred behavior in natural language."
        )
        action_lines = [
            "- Reply on this PR with the desired behavior in natural language.",
            f"- Decision needed: {prompt}",
            "- After that, the maintainer agent can apply the change and re-run review.",
        ]
    else:
        action_lines = [
            "- Maintainer changes are required before this PR can pass review.",
            "- Ask the maintainer agent to inspect this PR and apply the required fix.",
        ]

    actions = "\n".join(action_lines)
    return (
        f"<!-- {REVIEW_COMMENT_MARKER} -->\n"
        f"## AI Review Status\n\n"
        f"- Decision: `{decision.decision}`\n"
        f"- Summary: {decision.summary}\n\n"
        f"## Next Action\n"
        f"{actions}\n"
    )


def github_headers(token: str) -> dict[str, str]:
    return {
        "Accept": "application/vnd.github+json",
        "Authorization": f"Bearer {token}",
        "X-GitHub-Api-Version": "2022-11-28",
        "Content-Type": "application/json",
    }


def github_api_request(
    method: str,
    url: str,
    token: str,
    payload: JsonObject | None = None,
) -> JsonObject | list[JsonObject] | None:
    data = None
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
    request_object = request.Request(
        url=url, data=data, method=method, headers=github_headers(token)
    )
    with request.urlopen(request_object) as response:
        if response.status == 204:
            return None
        response_body = response.read().decode("utf-8")
    if not response_body:
        return None
    parsed = json.loads(response_body)
    if isinstance(parsed, dict):
        return parsed
    if isinstance(parsed, list):
        if all(isinstance(item, dict) for item in parsed):
            return parsed
    raise ValueError(f"unexpected GitHub API response from {url}")


def list_issue_comments(repository: str, pr_number: int, token: str) -> list[JsonObject]:
    url = f"https://api.github.com/repos/{repository}/issues/{pr_number}/comments?per_page=100"
    response = github_api_request("GET", url, token)
    if response is None:
        return []
    if not isinstance(response, list):
        raise ValueError("expected comment list from GitHub API")
    return response


def upsert_state_comment(repository: str, pr_number: int, token: str, body: str) -> None:
    upsert_marked_comment(
        repository=repository,
        pr_number=pr_number,
        token=token,
        marker=STATE_COMMENT_MARKER,
        body=body,
    )


def upsert_marked_comment(
    repository: str, pr_number: int, token: str, marker: str, body: str
) -> None:
    comments = list_issue_comments(repository, pr_number, token)
    for comment in comments:
        existing_body = get_optional_str(comment, "body")
        if existing_body is not None and marker in existing_body:
            comment_id = comment.get("id")
            if not isinstance(comment_id, int):
                raise ValueError("comment id missing from GitHub API response")
            url = f"https://api.github.com/repos/{repository}/issues/comments/{comment_id}"
            github_api_request("PATCH", url, token, {"body": body})
            return

    url = f"https://api.github.com/repos/{repository}/issues/{pr_number}/comments"
    github_api_request("POST", url, token, {"body": body})


def read_state_from_github(repository: str, pr_number: int, token: str) -> AutomationState | None:
    for comment in list_issue_comments(repository, pr_number, token):
        existing_body = get_optional_str(comment, "body")
        if existing_body is None:
            continue
        parsed_state = parse_existing_state(existing_body)
        if parsed_state is not None:
            return parsed_state
    return None


def add_labels(repository: str, pr_number: int, token: str, labels: tuple[str, ...]) -> None:
    if not labels:
        return
    url = f"https://api.github.com/repos/{repository}/issues/{pr_number}/labels"
    github_api_request("POST", url, token, {"labels": list(labels)})


def remove_label(repository: str, pr_number: int, token: str, label: str) -> None:
    encoded = parse.quote(label, safe="")
    url = f"https://api.github.com/repos/{repository}/issues/{pr_number}/labels/{encoded}"
    try:
        github_api_request("DELETE", url, token)
    except error.HTTPError as http_error:
        if http_error.code != 404:
            raise


def sync_labels(
    repository: str,
    token: str,
    context: PullRequestContext,
    phase: str,
    device_test_passed: bool,
    release_requested: bool,
) -> None:
    desired_labels = set(phase_to_labels(phase))
    if device_test_passed:
        desired_labels.add("device-test-passed")
    if release_requested:
        desired_labels.add("release-requested")

    labels_to_remove = set(PHASE_LABELS).union(PERSISTENT_LABELS) - desired_labels
    for label in sorted(labels_to_remove):
        remove_label(repository=repository, pr_number=context.number, token=token, label=label)
    add_labels(
        repository=repository,
        pr_number=context.number,
        token=token,
        labels=tuple(sorted(desired_labels)),
    )


def handle_sync_pr_state(
    event_path: Path,
    phase: str,
    selected_workflow: str | None,
    device_test_passed: bool,
    release_requested: bool,
    release_version: str | None,
    increment_autofix_attempts: bool,
    increment_review_attempts: bool,
) -> int:
    repository = os.environ.get("GITHUB_REPOSITORY")
    token = os.environ.get("GITHUB_TOKEN")
    if repository is None or not repository:
        raise ValueError("GITHUB_REPOSITORY is required")
    if token is None or not token:
        raise ValueError("GITHUB_TOKEN is required")

    event = load_json_file(event_path)
    pull_request = get_required_dict(event, "pull_request")
    pr_number = event.get("number")
    if not isinstance(pr_number, int):
        pr_number = pull_request.get("number")
    if not isinstance(pr_number, int):
        raise ValueError("pull request number missing from event")

    context = PullRequestContext(
        number=pr_number,
        head_ref="",
        head_sha="",
        base_ref="",
        base_sha="",
        labels=tuple(),
        body="",
        changed_files=tuple(),
    )

    previous_state = read_state_from_github(repository, pr_number, token)
    if previous_state is None:
        existing_state = AutomationState(
            phase=phase,
            autofix_attempts=0,
            review_attempts=0,
            selected_workflow=selected_workflow,
            release_requested=release_requested,
            release_version=release_version,
        )
    else:
        existing_state = AutomationState(
            phase=phase,
            autofix_attempts=previous_state.autofix_attempts
            + (1 if increment_autofix_attempts else 0),
            review_attempts=previous_state.review_attempts
            + (1 if increment_review_attempts else 0),
            selected_workflow=selected_workflow or previous_state.selected_workflow,
            release_requested=release_requested,
            release_version=release_version or previous_state.release_version,
        )

    sync_labels(
        repository=repository,
        token=token,
        context=context,
        phase=phase,
        device_test_passed=device_test_passed,
        release_requested=release_requested,
    )
    upsert_state_comment(
        repository=repository,
        pr_number=pr_number,
        token=token,
        body=build_state_comment(existing_state),
    )
    print_json(
        {
            "pull_request": pr_number,
            "phase": phase,
            "device_test_passed": device_test_passed,
            "release_requested": release_requested,
            "release_version": release_version,
            "selected_workflow": existing_state.selected_workflow,
            "autofix_attempts": existing_state.autofix_attempts,
            "review_attempts": existing_state.review_attempts,
        }
    )
    return 0


def handle_read_pr_state(event_path: Path, output_path: Path | None) -> int:
    repository = os.environ.get("GITHUB_REPOSITORY")
    token = os.environ.get("GITHUB_TOKEN")
    if repository is None or not repository:
        raise ValueError("GITHUB_REPOSITORY is required")
    if token is None or not token:
        raise ValueError("GITHUB_TOKEN is required")

    event = load_json_file(event_path)
    pull_request = get_required_dict(event, "pull_request")
    pr_number = event.get("number")
    if not isinstance(pr_number, int):
        pr_number = pull_request.get("number")
    if not isinstance(pr_number, int):
        raise ValueError("pull request number missing from event")

    state = read_state_from_github(repository, pr_number, token) or AutomationState(
        phase="unknown",
        autofix_attempts=0,
        review_attempts=0,
        selected_workflow=None,
        release_requested=False,
        release_version=None,
    )

    outputs = {
        "phase": state.phase,
        "autofix_attempts": str(state.autofix_attempts),
        "review_attempts": str(state.review_attempts),
        "selected_workflow": state.selected_workflow or "",
        "release_requested": "true" if state.release_requested else "false",
        "release_version": state.release_version or "",
    }
    write_github_output(output_path, outputs)
    print_json(
        {
            "pull_request": pr_number,
            "phase": state.phase,
            "autofix_attempts": state.autofix_attempts,
            "review_attempts": state.review_attempts,
            "selected_workflow": state.selected_workflow,
            "release_requested": state.release_requested,
            "release_version": state.release_version,
        }
    )
    return 0


def find_latest_review_report(runs_root: Path) -> Path:
    if not runs_root.is_dir():
        raise FileNotFoundError(f"runs root not found: {runs_root}")

    report_candidates = list(runs_root.glob("*/reports/review-report.md"))
    if not report_candidates:
        raise FileNotFoundError(f"no review-report.md under: {runs_root}")
    return max(report_candidates, key=lambda path: path.stat().st_mtime)


def resolve_review_report_path(runs_root: Path | None, report_path: Path | None) -> Path:
    if report_path is not None:
        if not report_path.is_file():
            raise FileNotFoundError(f"review report not found: {report_path}")
        return report_path
    if runs_root is None:
        raise ValueError("either --runs-root or --report-path is required")
    return find_latest_review_report(runs_root)


def parse_review_report_file(report_path: Path) -> ReviewDecision:
    values: dict[str, str] = {}
    with report_path.open("r", encoding="utf-8-sig") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("## "):
                break
            if ":" not in line:
                continue
            key, value = line.split(":", 1)
            values[key.strip()] = value.strip()

    decision = values.get("Decision")
    if decision not in {"APPROVE", "AUTO_FIXABLE", "NEEDS_DECISION", "REJECT"}:
        raise ValueError(f"unexpected Decision header in {report_path}")

    auto_fix_allowed = values.get("AutoFixAllowed", "false").lower() == "true"
    summary = values.get("Summary")
    if summary is None:
        raise ValueError(f"missing Summary header in {report_path}")
    user_decision_needed = values.get("UserDecisionNeeded", "no").lower() == "yes"
    user_prompt = values.get("UserPrompt")

    return ReviewDecision(
        decision=decision,
        auto_fix_allowed=auto_fix_allowed,
        summary=summary,
        user_decision_needed=user_decision_needed,
        user_prompt=user_prompt if user_prompt else None,
    )


def handle_parse_review_report(
    runs_root: Path | None, report_path: Path | None, output_path: Path | None
) -> int:
    resolved_report_path = resolve_review_report_path(runs_root, report_path)
    decision = parse_review_report_file(resolved_report_path)
    outputs = {
        "decision": decision.decision,
        "auto_fix_allowed": "true" if decision.auto_fix_allowed else "false",
        "summary": decision.summary,
        "user_decision_needed": "true" if decision.user_decision_needed else "false",
        "user_prompt": decision.user_prompt or "",
        "report_path": str(resolved_report_path),
    }
    write_github_output(output_path, outputs)
    print_json(
        {
            "decision": decision.decision,
            "auto_fix_allowed": decision.auto_fix_allowed,
            "summary": decision.summary,
            "user_decision_needed": decision.user_decision_needed,
            "user_prompt": decision.user_prompt,
            "report_path": str(resolved_report_path),
        }
    )
    return 0


def handle_prepare_release_local(repo_root: Path) -> int:
    version = read_release_version(repo_root)
    if version is None:
        raise ValueError("VERSION was not found in Makefile")

    release_notes_path = repo_root / f"RELEASE_NOTES_v{version}.md"
    if not release_notes_path.is_file():
        raise FileNotFoundError(f"release notes missing: {release_notes_path}")

    asset_paths = (
        repo_root / "streamu.3dsx",
        repo_root / "streamu.cia",
        repo_root / "server" / "dist" / "StreaMu-Server.zip",
    )
    missing_assets = [str(path) for path in asset_paths if not path.is_file()]
    if missing_assets:
        raise FileNotFoundError(f"release assets missing: {', '.join(missing_assets)}")

    release_plan = ReleasePlan(
        version=version,
        tag_name=f"v{version}",
        release_title=f"StreaMu v{version}",
        release_notes_path=str(release_notes_path),
        asset_paths=tuple(str(path) for path in asset_paths),
        target_sha=read_head_sha(repo_root),
    )
    print_json(
        {
            "version": release_plan.version,
            "tag_name": release_plan.tag_name,
            "release_title": release_plan.release_title,
            "release_notes_path": release_plan.release_notes_path,
            "asset_paths": list(release_plan.asset_paths),
            "target_sha": release_plan.target_sha,
        }
    )
    return 0


def handle_publish_review_comment(
    event_path: Path,
    decision: str,
    summary: str,
    user_decision_needed: bool,
    user_prompt: str | None,
) -> int:
    repository = os.environ.get("GITHUB_REPOSITORY")
    token = os.environ.get("GITHUB_TOKEN")
    if repository is None or not repository:
        raise ValueError("GITHUB_REPOSITORY is required")
    if token is None or not token:
        raise ValueError("GITHUB_TOKEN is required")

    event = load_json_file(event_path)
    pull_request = get_required_dict(event, "pull_request")
    pr_number = event.get("number")
    if not isinstance(pr_number, int):
        pr_number = pull_request.get("number")
    if not isinstance(pr_number, int):
        raise ValueError("pull request number missing from event")

    review_decision = ReviewDecision(
        decision=decision,
        auto_fix_allowed=decision == "AUTO_FIXABLE",
        summary=summary,
        user_decision_needed=user_decision_needed,
        user_prompt=user_prompt if user_prompt else None,
    )
    upsert_marked_comment(
        repository=repository,
        pr_number=pr_number,
        token=token,
        marker=REVIEW_COMMENT_MARKER,
        body=build_review_comment(review_decision),
    )
    print_json(
        {
            "pull_request": pr_number,
            "decision": decision,
            "user_decision_needed": user_decision_needed,
        }
    )
    return 0


def handle_build_autofix_task(
    report_path: Path, output_path: Path, pr_number: int, base_sha: str
) -> int:
    decision = parse_review_report_file(report_path)
    if decision.decision != "AUTO_FIXABLE":
        raise ValueError("build-autofix-task requires an AUTO_FIXABLE report")

    report_text = report_path.read_text(encoding="utf-8-sig")
    task_body = (
        f"# Auto Fix Task\n"
        f"PR: #{pr_number}\n"
        f"Base SHA: {base_sha}\n"
        f"Use only the findings from the review report below.\n"
        f"Do not add features, change UX behavior, or modify release metadata.\n\n"
        f"{report_text}\n"
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(task_body, encoding="utf-8")
    print_json(
        {
            "task_path": str(output_path),
            "pr_number": pr_number,
            "base_sha": base_sha,
        }
    )
    return 0


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        if args.command == "inspect-pr":
            return handle_inspect_pr(
                event_path=args.event_path.resolve(),
                repo_root=args.repo_root.resolve(),
                output_path=args.write_github_output.resolve()
                if args.write_github_output
                else None,
            )
        if args.command == "inspect-local":
            return handle_inspect_local(
                repo_root=args.repo_root.resolve(),
                scope_paths=normalize_scope_paths(args.scope_path),
            )
        if args.command == "apply-deterministic-autofix":
            return handle_apply_deterministic_autofix(
                repo_root=args.repo_root.resolve(),
                changed_files_json=args.changed_files_json,
                output_path=args.write_github_output.resolve()
                if args.write_github_output
                else None,
            )
        if args.command == "run-local-checks":
            return handle_run_local_checks(
                repo_root=args.repo_root.resolve(),
                skip_build=args.skip_build,
                scope_paths=normalize_scope_paths(args.scope_path),
            )
        if args.command == "classify-publishable":
            return handle_classify_publishable(paths_json=args.paths_json)
        if args.command == "run-static-checks":
            return handle_run_static_checks(
                repo_root=args.repo_root.resolve(),
                ruff_targets_json=args.ruff_targets_json,
                py_compile_targets_json=args.py_compile_targets_json,
                mypy_targets_json=args.mypy_targets_json,
            )
        if args.command == "sync-pr-state":
            return handle_sync_pr_state(
                event_path=args.event_path.resolve(),
                phase=args.phase,
                selected_workflow=args.selected_workflow or None,
                device_test_passed=parse_bool_string(args.device_test_passed),
                release_requested=parse_bool_string(args.release_requested),
                release_version=args.release_version or None,
                increment_autofix_attempts=args.increment_autofix_attempts,
                increment_review_attempts=args.increment_review_attempts,
            )
        if args.command == "read-pr-state":
            return handle_read_pr_state(
                event_path=args.event_path.resolve(),
                output_path=args.write_github_output.resolve()
                if args.write_github_output
                else None,
            )
        if args.command == "parse-review-report":
            return handle_parse_review_report(
                runs_root=args.runs_root.resolve() if args.runs_root else None,
                report_path=args.report_path.resolve() if args.report_path else None,
                output_path=args.write_github_output.resolve()
                if args.write_github_output
                else None,
            )
        if args.command == "publish-review-comment":
            return handle_publish_review_comment(
                event_path=args.event_path.resolve(),
                decision=args.decision,
                summary=args.summary,
                user_decision_needed=parse_bool_string(args.user_decision_needed),
                user_prompt=args.user_prompt or None,
            )
        if args.command == "build-autofix-task":
            return handle_build_autofix_task(
                report_path=args.report_path.resolve(),
                output_path=args.output_path.resolve(),
                pr_number=args.pr_number,
                base_sha=args.base_sha,
            )
        if args.command == "prepare-release-local":
            return handle_prepare_release_local(repo_root=args.repo_root.resolve())
        raise ValueError(f"unsupported command: {args.command}")
    except (
        FileNotFoundError,
        ValueError,
        json.JSONDecodeError,
        subprocess.CalledProcessError,
        error.HTTPError,
        error.URLError,
    ) as problem:
        print(f"error: {problem}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
