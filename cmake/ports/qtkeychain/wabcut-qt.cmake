# The Qt the port is built against must be the Qt the app links (two Qt6Core DLLs in one process break). This file
# is part of the port, so its content is in the package ABI hash: when Wabcut moves to another Qt, bump the version
# here and every machine rebuilds QtKeychain instead of reusing a binary built for the old Qt.
set(WABCUT_KEYCHAIN_QT_VERSION "6.11.3")
