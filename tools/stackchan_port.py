"""スタックチャンの USB ポートを探す。環境変数 STACKCHAN_PORT があればそれを使う。"""
import glob, os


def find_port() -> str:
    if os.environ.get("STACKCHAN_PORT"):
        return os.environ["STACKCHAN_PORT"]
    for pat in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        ports = sorted(glob.glob(pat))
        if ports:
            return ports[0]
    raise SystemExit("スタックチャンが見つかりません。USB-C でつないで、STACKCHAN_PORT を指定してください")


def load_env(path=None) -> dict:
    """リポジトリ直下の .env を読む (KEY=VALUE、前後の引用符と空白は外す)"""
    path = path or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".env")
    env = {}
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip('"').strip("'").strip()
    return env
