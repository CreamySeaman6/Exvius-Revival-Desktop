"""engine.py <server repository> <data folder> <save folder> : the server engine without the game.

Sets the engine up as the launcher does (that repository's docker-compose.yml switches, the desktop's own), then
runs the Python it reads from standard input with the engine's modules importable. For looking things up and for
trying the server's own steps on a save. Give it a COPY of a save folder: the engine may write to it.

    cp -r build/linux-layout/save /tmp/save-copy
    echo "from ffbe_gacha import unit_name; print(unit_name('100000315'))" | python3 test/engine.py ~/Exvius-revival-ios ~/data /tmp/save-copy
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    dev, data, save = (os.path.abspath(p) for p in sys.argv[1:4])
    home = os.path.join(os.path.dirname(save), 'home-for-engine')
    os.makedirs(os.path.join(home, 'server'), exist_ok=True)
    shutil.copy(os.path.join(dev, 'ios-server', 'docker-compose.yml'), os.path.join(home, 'server'))
    os.environ.update({'EXVIUS_DESKTOP_HOME': home, 'FFBE_ENGINE': os.path.join(dev, 'source', 'server'), 'FFBE_DATA_DIR': data,
                       'FFBE_SERVER_ROOT': save, 'EXVIUS_IOS_HCA': '0', 'EXVIUS_IOS_HCA_PLAIN': '0'})
    sys.path.insert(0, os.path.join(HERE, '..', 'app'))
    import launcher
    for key, value in launcher.release_environment().items():
        os.environ.setdefault(key, value)
    sys.path.insert(0, os.path.join(dev, 'ios-server'))
    import serve
    serve.configure_engine()
    exec(sys.stdin.read(), {'__name__': '__engine__'})  # noqa: S102 - a test aid: runs what it is given
    return 0


if __name__ == '__main__':
    sys.exit(main())
