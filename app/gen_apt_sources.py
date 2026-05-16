import os

DEBIAN_RELEASE = os.environ.get('DEBIAN_RELEASE', 'trixie')
DEBIAN_MIRROR = os.environ.get('DEBIAN_MIRROR', 'https://deb.debian.org/debian')
DEBIAN_SECURITY_MIRROR = os.environ.get('DEBIAN_SECURITY_MIRROR', 'https://security.debian.org/debian-security')

repos_file = [
    f'deb {DEBIAN_MIRROR} {DEBIAN_RELEASE} main contrib non-free non-free-firmware',
    f'deb {DEBIAN_MIRROR} {DEBIAN_RELEASE}-updates main contrib non-free non-free-firmware',
    f'deb {DEBIAN_SECURITY_MIRROR} {DEBIAN_RELEASE}-security main contrib non-free non-free-firmware',
]

output_dir = os.path.join(os.environ['BUILT_PRODUCTS_DIR'], os.environ['CONTENTS_FOLDER_PATH'])
os.makedirs(output_dir, exist_ok=True)

with open(os.path.join(output_dir, 'repositories.txt'), 'w') as f:
    for line in repos_file:
        print(line, file=f)
