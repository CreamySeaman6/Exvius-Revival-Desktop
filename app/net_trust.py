"""HTTPS certificate checks for everything this program downloads (the game data, banner art).

The bundled Python only trusts the root certificates already sitting in the Windows store. Windows itself fetches
missing roots on demand, so a fresh PC may not have the one a download server chains to yet; and antivirus HTTPS
scanning or a network filter adds a root of its own that the browsers accept. Either way a download fails with
CERTIFICATE_VERIFY_FAILED although the same address opens in a browser.

`apply()` makes Python check certificates the way the browsers on this computer do: `truststore` hands the check to
the system (Windows fetches the missing root itself). If that library is missing, the system's store is used
together with the `certifi` list of roots. Certificates are verified in every case.
"""
from __future__ import annotations

_applied = ''


def apply() -> str:
    """Returns which check is in use: 'system', 'store and bundle', or 'default' (nothing could be changed)."""
    global _applied
    if _applied:
        return _applied
    try:
        import truststore
        truststore.inject_into_ssl()
        _applied = 'system'
        return _applied
    except Exception:  # noqa: BLE001 - the library is missing or the system refused; try the next way
        pass
    try:
        import ssl
        import urllib.request

        import certifi
        context = ssl.create_default_context()                  # what the Windows store holds (an antivirus root too)...
        context.load_verify_locations(cafile=certifi.where())   # ...plus the roots it may not have fetched yet
        urllib.request.install_opener(urllib.request.build_opener(urllib.request.HTTPSHandler(context=context)))
        _applied = 'store and bundle'
        return _applied
    except Exception:  # noqa: BLE001
        _applied = 'default'
        return _applied
