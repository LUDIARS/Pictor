#!/usr/bin/env python3
"""SHaRC 拡張デモ用アセットのダウンローダ。

spec: pictor-sharc-ext-design.md §6 (技術デモ構成)

  - core セット (既定): D2 計測用のスキャンモデル (数十 MB)
      Stanford Dragon / Happy Buddha (Stanford 3D Scanning Repository)
      Lee Perry-Smith head (McGuire Computer Graphics Archive, CC-BY)
  - full セット (--full): Hero / 負荷シーン (数 GB, ORCA)
      Amazon Lumberyard Bistro / Emerald Square / Zero-Day

計測対象はプロシージャル + スキャンのみ (AI 生成は厚み精度・albedo
焼き込みで不可, spec §6)。 props の AI 生成 / Sketchfab 調達は別途手動。

配置先: demo/assets/sharc/ (gitignore 済み、 working copy ローカル)。
使い方:
    python tools/download_sharc_demo_assets.py           # core のみ
    python tools/download_sharc_demo_assets.py --full    # ORCA 大物含む
    python tools/download_sharc_demo_assets.py --list    # 一覧表示のみ
"""

from __future__ import annotations

import argparse
import dataclasses
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ASSET_ROOT = Path(__file__).resolve().parent.parent / "demo" / "assets" / "sharc"

USER_AGENT = "PictorSharcAssetFetcher/1.0"


@dataclasses.dataclass(frozen=True)
class Asset:
    """1 ダウンロード = 1 アセット。 dest はアーカイブ展開先サブディレクトリ。"""

    name: str
    url: str
    dest: str
    approx_size: str
    license_note: str
    demo_ids: str
    full_only: bool = False


ASSETS: list[Asset] = [
    Asset(
        name="Stanford Dragon (reconstruction)",
        url="http://graphics.stanford.edu/pub/3Dscanrep/dragon/dragon_recon.tar.gz",
        dest="dragon",
        approx_size="~11MB",
        license_note="Stanford 3D Scanning Repository (研究利用可, 出典明記)",
        demo_ids="D2",
    ),
    Asset(
        name="Happy Buddha (reconstruction)",
        url="http://graphics.stanford.edu/pub/3Dscanrep/happy/happy_recon.tar.gz",
        dest="buddha",
        approx_size="~17MB",
        license_note="Stanford 3D Scanning Repository (研究利用可, 出典明記)",
        demo_ids="D2",
    ),
    Asset(
        name="Lee Perry-Smith head",
        url="https://casual-effects.com/g3d/data10/research/model/lpshead/lpshead.zip",
        dest="lpshead",
        approx_size="~64MB",
        license_note="CC BY 3.0 (Infinite Realities / McGuire CG Archive)",
        demo_ids="D2",
    ),
    Asset(
        name="Amazon Lumberyard Bistro",
        url="https://casual-effects.com/g3d/data10/research/model/bistro/bistro.zip",
        dest="bistro",
        approx_size="~2.4GB",
        license_note="CC BY 4.0 (Amazon Lumberyard / ORCA)",
        demo_ids="Hero",
        full_only=True,
    ),
    Asset(
        name="NVIDIA Emerald Square",
        url="https://casual-effects.com/g3d/data10/research/model/emerald_square/emerald_square.zip",
        dest="emerald_square",
        approx_size="~1.2GB",
        license_note="CC BY 4.0 (NVIDIA / ORCA)",
        demo_ids="負荷",
        full_only=True,
    ),
    Asset(
        name="Zero-Day",
        url="https://casual-effects.com/g3d/data10/research/model/ZeroDay/ZeroDay.zip",
        dest="zeroday",
        approx_size="~700MB",
        license_note="CC BY 4.0 (Beeple / ORCA)",
        demo_ids="負荷",
        full_only=True,
    ),
]


_last_reported_mb = -1


def _report_progress(count: int, block_size: int, total: int) -> None:
    # 非 TTY (CI / ログ収集) で行が氾濫しないよう 4MB 刻みでのみ出す。
    global _last_reported_mb
    done_mb = count * block_size // (1 << 20)
    if done_mb // 4 == _last_reported_mb // 4:
        return
    _last_reported_mb = done_mb
    if total > 0:
        pct = min(count * block_size * 100 // total, 100)
        print(f"  {done_mb}MB / {total // (1 << 20)}MB ({pct}%)", flush=True)
    else:
        print(f"  {done_mb}MB", flush=True)


def _extract(archive: Path, dest: Path) -> None:
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    elif archive.name.endswith((".tar.gz", ".tgz")):
        with tarfile.open(archive, "r:gz") as t:
            try:
                t.extractall(dest, filter="data")
            except TypeError:  # Python < 3.12 は filter 未対応
                t.extractall(dest)
    else:
        raise ValueError(f"未対応アーカイブ形式: {archive.name}")


def download_asset(asset: Asset) -> bool:
    dest = ASSET_ROOT / asset.dest
    if dest.exists() and any(dest.iterdir()):
        print(f"[skip] {asset.name}: 既に存在 ({dest})")
        return True

    dest.mkdir(parents=True, exist_ok=True)
    archive = dest / Path(asset.url).name
    print(f"[get ] {asset.name} ({asset.approx_size}) <- {asset.url}")
    opener = urllib.request.build_opener()
    opener.addheaders = [("User-Agent", USER_AGENT)]
    urllib.request.install_opener(opener)
    try:
        urllib.request.urlretrieve(asset.url, archive, _report_progress)
        print()
        _extract(archive, dest)
        archive.unlink()
        print(f"[ ok ] {asset.name} -> {dest}")
        return True
    except Exception as e:  # noqa: BLE001 — 失敗アセットを列挙して続行する
        print(f"\n[fail] {asset.name}: {e}")
        # 失敗アーカイブは消して次回リトライ可能にする
        if archive.exists():
            archive.unlink()
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--full", action="store_true",
                        help="ORCA 大容量シーン (数 GB) も落とす")
    parser.add_argument("--list", action="store_true", help="一覧表示のみ")
    parser.add_argument("--only", metavar="DEST",
                        help="dest 名指定で 1 アセットのみ (例: dragon)")
    args = parser.parse_args()

    targets = [a for a in ASSETS
               if (args.full or not a.full_only)
               and (args.only is None or a.dest == args.only)]

    if args.list or not targets:
        for a in ASSETS:
            tag = "full" if a.full_only else "core"
            print(f"[{tag}] {a.dest:16} {a.approx_size:8} {a.name} "
                  f"({a.demo_ids}) — {a.license_note}")
        return 0

    failures = [a.name for a in targets if not download_asset(a)]
    if failures:
        print(f"\n失敗: {len(failures)} 件: {', '.join(failures)}")
        return 1
    print("\n全アセット取得完了")
    return 0


if __name__ == "__main__":
    sys.exit(main())
