"""This module holds state.

Modules in this package may modify state.

Erasing the contents of each container below will completely zero out
the currently held state of avalon-core.

"""

_registered_root = {"_": ""}
_registered_host = {"_": None}
_registered_config = {"_": None}

Session = {}
