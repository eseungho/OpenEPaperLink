"""Gzips wwwroot/* into data/www/*.gz.

The output is deterministic: text files are compressed with LF line endings, whatever the checkout
uses (Git on Windows checks them out with CRLF), and the gzip header has no file name or time stamp.
A .gz is only rewritten when its decompressed content differs from the source, so after a run only
the .gz files whose source really changed differ, and only those go in a commit. Comparing the
content instead of the compressed bytes keeps runs on machines with a different zlib from rewriting
every file.

The firmware filesystem (littlefs.bin) and the OTA file list are built from the committed data/www,
so run this after every change in wwwroot and commit the .gz files with it.

    python gzip_wwwfiles.py          regenerate data/www
    python gzip_wwwfiles.py --check  only compare; exit 1 when data/www is out of date (used by CI)
"""
import gzip
import os
import sys

SOURCE_FOLDER = "wwwroot"
DESTINATION_FOLDER = "data/www"


def normalized(data):
    # a NUL byte means binary (favicon.ico): leave it as it is
    if b"\0" in data:
        return data
    return data.replace(b"\r\r\n", b"\n").replace(b"\r\n", b"\n")


def compressed_content(path):
    try:
        with open(path, "rb") as f:
            return gzip.decompress(f.read())
    except (OSError, EOFError):
        return None


def gzip_files(source_folder, destination_folder, check_only=False):
    """Returns a list of problems: .gz files that are out of date (check mode) or have no source."""
    os.makedirs(destination_folder, exist_ok=True)
    problems = []
    expected = set()
    for file in sorted(os.listdir(source_folder)):
        source_file_path = os.path.join(source_folder, file)
        if not os.path.isfile(source_file_path):
            continue
        expected.add(file + ".gz")
        destination_file_path = os.path.join(destination_folder, file + ".gz")

        with open(source_file_path, "rb") as f_in:
            content = normalized(f_in.read())

        if compressed_content(destination_file_path) == content:
            continue
        if check_only:
            problems.append(f"out of date: {destination_file_path}")
            continue
        print(f"Gzipping: {file}")
        with open(destination_file_path, "wb") as f_out:
            f_out.write(gzip.compress(content, compresslevel=9, mtime=0))

    for file in sorted(os.listdir(destination_folder)):
        if file.endswith(".gz") and file not in expected:
            problems.append(f"no source in {source_folder}: {os.path.join(destination_folder, file)}")
    return problems


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    check_only = "--check" in sys.argv[1:]
    problems = gzip_files(SOURCE_FOLDER, DESTINATION_FOLDER, check_only)
    for problem in problems:
        print(problem)
    if check_only and problems:
        print("data/www does not match wwwroot: run 'python gzip_wwwfiles.py' in ESP32_AP-Flasher "
              "and commit the .gz files.")
        sys.exit(1)
