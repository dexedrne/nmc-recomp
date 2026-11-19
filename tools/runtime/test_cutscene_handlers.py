#!/usr/bin/env python3
"""Static drift check for the PC cutscene cases reviewed in implementation notes."""
import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PE_SHA = '9002b2de6a1f91a790bd0673de125d1cf833f7942bfec827cdcf6ba64d5849ed'

# Complete lifted bodies include address computations and outgoing calls.
# Pin the shared dispatchers as well as the case-specific handlers.
HANDLERS = {
    26: {
        '14c3f0': 'c896516825dd4492b29524b6e35ff11905ebca330dff48e8c5eb1b58a53b5008',
        '14c3b0': 'ea8bb9663d417e17fe8995bb253971bc8fb1e8ab5b9bfea8027e098237311ceb',
        '14c380': 'e5531ea7516564b222dbd7f37f48c2dc98ef6786b22ffdd6a071e325b0a48b38',
        '14c780': 'f1dc22992530243fcfbd046361bdf26730ef7629f7d4d0fa75ca0e961571cd00',
    },
    27: {
        '156df0': 'da564b32e5644663b47886d10afebe5da584e6298e3a55d1837cbade65dd4f44',
        '156de0': '84df84b23f379eb7e8a5a6de39d65050966ed7d74bf6cbf3bb56bedc93ce7047',
        '156c90': '3badd0003c76096ea328ed7aca82d4f9c56658ab6cb96f6937b9b5090356543b',
        '156ce0': 'b6e3ac9afa3783d7c7c9f4d5438d466c1ecb8e22f3441cc8b513dadc6645bdbe',
        '156d00': '0f49529f411272188caededc6a556770f4b117a05276fec0e3237e5379c0d0ad',
        '156d90': '33811f92fe5ba2d13f5290368465b3526c57aefa45f483e8bceb018f8f0ecbd0',
        '156dc0': '005c0430375e65ecc36ff0e24eba53486a2f550428706cea077342f95dbf8dfc',
        '156dd0': 'd39156cb23b6cfcf0072ed659386d1268c4f996c0d1d84b45efba04ea1d1c1b0',
    },
    28: {
        '160a50': 'e137474a766920628a0f3b9f815c5b78c34c446f7b44085b9b9481a642b4473d',
        '160a70': '16447d502b2f41286aa278117d88699a931aa099d3c5b76933082bb46609410c',
    },
    32: {
        '19ddd0': '550c764fba7775afa7b8479df7192a9c87f0537a6b85068be2a79c8c17461dfa',
    },
    57: {
        '2c85b0': 'b73ba7f1af464a8fcb8d0456d384567323a6ab25d52d66009487ae6fe73b53f4',
        '2c87f0': 'aa60645d43cac5938fb5bbfaff2ddc9c5ae97b4ac1593b79ec582547984697a4',
    },
    58: {
        '2d37b0': 'd63d91fb8e3d5a84a244591b16150562a6a6a42aac1b0e4b7c5ba46707cd5eb6',
        '2cc4b0': '7c148bfc19c8626d57643c818143d034c0067900ed856172a6dbf689c6baef71',
        '2ceb20': 'dfe1c8e4839d9dad7eda6fb9692d9f615ae05b450cd1f4c73d89f93298688f85',
        '2d3570': 'e0b774901168b0c95fac41f96d5c6415519b8fe11f3a268554164d02615ab95d',
        '2d61f0': '18960b2d25ea3a7e9abeb164e62a85442aa3548994250f5cb89df7c500e8b73a',
        '2d42a0': '2953ac16da6e5de766c0e6197cad2c6d13ccb89132e744cc6e98062fef8111eb',
        '2ce470': '669a20c8d4639d3842f7f0278a9230c6d9f19dddf2b97736bf0dc649a4d055d3',
        '2cea40': 'f8e040cc0e40ca86b6d6f0ec93b2775b253fb5ea165f25d19f9acb57f22c98ee',
        '2cbe30': 'e02f7ff7c5adb1f0703b3bec4d47b77347f06c76f935f00aaf1a2874cf9800b6',
        '2cc0b0': '68c6e5e82d6ca246ae62d605c923580140344216b0105002a91890f1c0a446f6',
        '2c8f40': 'ead0c27fafcf14e32363475484df6f9ab77477c435309cad48a73698cc013bb9',
        '2cb5d0': '6e44f9d658489833b618b3d3260abaa96c15b51e22c14f206b4616ef187acc04',
        '2cc180': '557062db8bee70b4f83b40ba07b8bbe018c72ecf990f1e911ac63c8ca3579312',
        '2d2620': 'a132dbba76358e56f2caee26febbe602ed13e457271ab67b3fd78c1d284ed95e',
        '2d2660': '097a4403f656ec20eb196cef8ce12aeddc60c4a21648f29f7eea924d717ab8c6',
        '2d3d40': 'e7678df33761792501a6e94ad42e1acb5e57cb21d6432a0850d5bbde725f809a',
        '2d4580': 'fd50b2bf865db88c1cc27d585e40bfae624c2e742d1ef813c122c95ff65be199',
    },
    59: {
        '2daa60': 'a948c4be386784c1476ba579582d5786cff968223cc1a797bf1a224464d8afd9',
        '2daa50': '8dd3342590dd32f3369fef7f18fa632ed26e6ccc5a91b2869cdb774234be1a69',
        '2dab00': 'ae30ba194c874712f511781cdc7d7864b65ecd555b3c8365bb0e22c2af1b70a3',
        '2da730': '48b98e6d5e4f43f2a3e13e860e5f0fd7ad6680987a994baba5754a48686371a6',
    },
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    if digest((ROOT / 'pc/kh2fm.exe').read_bytes()) != PE_SHA:
        raise SystemExit('cutscenes-handler: PC image and dispatch tables require review')
    for shard, functions in HANDLERS.items():
        source = (ROOT / 'lifted/shards' / f'shard_{shard:04d}.c').read_bytes()
        for rva, expected in functions.items():
            body = re.search(rb'^void f_' + rva.encode() + rb'\(KCtx \*g\)\{.*?^\}\n\}\n',
                             source, re.MULTILINE | re.DOTALL)
            if body is None or digest(body[0]) != expected:
                raise SystemExit('cutscenes-handler: re-review writes and calls of f_' + rva)
    print('cutscenes-handler: PC image and all reviewed lifted bodies match')


if __name__ == '__main__':
    main()
