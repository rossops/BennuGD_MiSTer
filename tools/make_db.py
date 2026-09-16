#!/usr/bin/env python3
"""Build the MiSTer Downloader ("update_all") database for this core.

    make_db.py [--repo user/repo] [--branch main] [--out db.json.zip]

Lists the newest releases/BennuGD_<date>.rbf and the game entries as
_Other/_BennuGD/..., the ARM side (releases/bennugd, stripped, plus the
scripts in dist/) as bennugd/..., and the OSD installer as
Scripts/BennuGD_install.sh, each with MD5 and size and a raw GitHub URL.
bennugd.cfg is marked overwrite=false so a user's edits survive updates.
Users add to /media/fat/downloader.ini:

    [<user>/<repo>]
    db_url = https://raw.githubusercontent.com/<user>/<repo>/<branch>/db.json.zip

run Update All, then run Scripts -> BennuGD_install once (MiSTer.ini and
user-startup.sh are not something the downloader can edit). Regenerate and
commit db.json.zip whenever any of these files change.
"""
import argparse, glob, hashlib, json, os, re, time, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default="rossops/BennuGD_MiSTer")
    ap.add_argument("--branch", default="main")
    ap.add_argument("--out", default=os.path.join(ROOT, "db.json.zip"))
    a = ap.parse_args()
    raw = f"https://raw.githubusercontent.com/{a.repo}/{a.branch}/"

    def entry(card_path, repo_path, **extra):
        p = os.path.join(ROOT, repo_path)
        e = {"hash": md5(p), "size": os.path.getsize(p), "url": raw + repo_path.replace(" ", "%20")}
        e.update(extra)
        files[card_path] = e

    files = {}
    folders = {"_Other": {}, "_Other/_BennuGD": {}, "bennugd": {}, "bennugd/mgl": {},
               "Scripts": {}, "games": {}, "games/BennuGD": {}}

    rbfs = sorted(glob.glob(os.path.join(ROOT, "releases", "BennuGD_*.rbf")),
                  key=lambda p: re.search(r"_(\d{8})\.rbf$", p).group(1))
    if not rbfs:
        raise SystemExit("no releases/BennuGD_<date>.rbf")
    rbf = os.path.basename(rbfs[-1])
    entry(f"_Other/_BennuGD/{rbf}", f"releases/{rbf}")
    for mgl in sorted(glob.glob(os.path.join(ROOT, "dist", "mgl", "*.mgl"))):
        name = os.path.basename(mgl)
        entry(f"_Other/_BennuGD/{name}", f"dist/mgl/{name}")
        entry(f"bennugd/mgl/{name}", f"dist/mgl/{name}")
    entry("bennugd/bennugd", "releases/bennugd", reboot=False)
    entry("bennugd/bennugd-launcherd.sh", "dist/bennugd-launcherd.sh")
    entry("bennugd/install.sh", "dist/install.sh")
    entry("bennugd/bennugd.cfg", "dist/bennugd.cfg", overwrite=False)
    entry("Scripts/BennuGD_install.sh", "dist/BennuGD_install.sh")

    db = {
        "db_id": a.repo.lower(),
        "db_url": raw + "db.json.zip",
        "timestamp": int(time.time()),
        "base_files_url": raw,
        "files": files,
        "folders": folders,
        "default_options": {},
        "zips": {},
    }
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("db.json", json.dumps(db, indent=1, sort_keys=True))
    print(f"{a.out}: {len(files)} files, core {rbf}")


if __name__ == "__main__":
    main()
