"""Run CTest with integration retries and summarize recoveries in GitHub Actions."""

import os
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path


def recovered_tests(log):
    # Parallel CTest interleaves attempts. Match starts and passes by test ID.
    start = re.compile(r"^\s*Start\s+(\d+):")
    passed = re.compile(r"^\s*(?:\d+/\d+\s+)?Test\s+#(\d+): (.+?)\s+\.+\s+Passed\s+[\d.]+ sec\s*$")
    attempts = Counter()
    recovered = {}
    for line in log:
        match = start.match(line)
        if match:
            attempts[match[1]] += 1
        match = passed.match(line)
        if match and attempts[match[1]] > 1:
            recovered[match[2]] = attempts[match[1]]
    return recovered


def main():
    log_path = Path(sys.argv[1])
    result = subprocess.run([
        "ctest", "-L", "integration", "--repeat", "until-pass:3",
        "--output-log", str(log_path), *sys.argv[2:],
    ])
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path and log_path.is_file():
        with log_path.open(encoding="utf-8", errors="replace") as log:
            recovered = recovered_tests(log)
        if recovered:
            with open(summary_path, "a", encoding="utf-8") as summary:
                print("### Integration tests that passed on retry\n", file=summary)
                for name, count in sorted(recovered.items()):
                    print(f"- `{name}` passed on attempt {count}", file=summary)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
