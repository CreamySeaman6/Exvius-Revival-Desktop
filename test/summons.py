"""summons.py <server repository> <rotation 0|1> <replay 0|1> [data folder] : which summons the server shows.

Runs the server engine without the game: the launcher's server environment (from that repository's
ios-server/docker-compose.yml), EXVIUS_SUMMON_ROTATION and EXVIUS_REPLAY as given, then lists what
`gacha_archive.visible_banners` returns for a player with no tickets. The save it needs goes to a new temporary folder.

    python3 test/summons.py ~/Exvius-revival-ios 0 0 ~/data

Written in the sixth session to compare the server shipped in 0.1.2 (ae07888: rotation off showed 16 of 1,123
banners) with 36e3bde (1,047 of 1,324).
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    dev, rotation, replay = os.path.abspath(sys.argv[1]), sys.argv[2], sys.argv[3]
    data = os.path.abspath(sys.argv[4] if len(sys.argv) > 4 else os.path.join(HERE, '..', 'build', 'data'))
    work = tempfile.mkdtemp(prefix='summons-')
    try:
        os.makedirs(os.path.join(work, 'home', 'server'))
        shutil.copy(os.path.join(dev, 'ios-server', 'docker-compose.yml'), os.path.join(work, 'home', 'server'))
        os.environ.update({'EXVIUS_DESKTOP_HOME': os.path.join(work, 'home'), 'FFBE_ENGINE': os.path.join(dev, 'source', 'server'),
                           'FFBE_DATA_DIR': data, 'FFBE_SERVER_ROOT': os.path.join(work, 'save'),
                           'EXVIUS_IOS_HCA': '0', 'EXVIUS_IOS_HCA_PLAIN': '0'})
        sys.path.insert(0, os.path.join(HERE, '..', 'app'))
        import launcher
        for key, value in launcher.release_environment().items():
            os.environ.setdefault(key, value)
        os.environ['EXVIUS_SUMMON_ROTATION'] = rotation
        os.environ['EXVIUS_REPLAY'] = replay
        sys.path.insert(0, os.path.join(dev, 'ios-server'))
        import serve
        serve.configure_engine()
        import ffbe_gacha
        import gacha_archive
        banners = ffbe_gacha.all_banners()
        shown = gacha_archive.visible_banners(None, banners)
        print(f'rotation={rotation} replay={replay}: {len(banners)} banners known, {len(shown)} shown')
        for banner in shown:
            print(f"  {banner.get('id')}  {banner.get('name') or ''}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
