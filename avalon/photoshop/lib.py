import json
import contextlib
import subprocess
import os
import sys
import queue
import importlib
import time
import traceback

from ..tools import html_server
from ..vendor.Qt import QtWidgets
from ..tools import workfiles

self = sys.modules[__name__]
self.callback_queue = None


def execute_in_main_thread(func_to_call_from_main_thread):
    self.callback_queue.put(func_to_call_from_main_thread)


def main_thread_listen():
    callback = self.callback_queue.get()
    callback()


def show(module_name):
    """Call show on "module_name".

    This allows to make a QApplication ahead of time and always "exec_" to
    prevent crashing.

    Args:
        module_name (str): Name of module to call "show" on.
    """
    # Need to have an existing QApplication.
    app = QtWidgets.QApplication.instance()
    if not app:
        app = QtWidgets.QApplication(sys.argv)

    # Import and show tool.
    tool_module = importlib.import_module("avalon.tools." + module_name)

    if "loader" in module_name:
        tool_module.show(use_context=True)
    else:
        tool_module.show()

    # QApplication needs to always execute.
    app.exec_()


def get_com_objects():
    """Wrapped com objects.

    This could later query whether the platform is Windows or Mac.
    """
    from . import com_objects
    return com_objects


def Dispatch(application):
    """Wrapped Dispatch function.

    This could later query whether the platform is Windows or Mac.

    Args:
        application (str): Application to dispatch.
    """
    from win32com.client.gencache import EnsureDispatch
    return EnsureDispatch(application)


def app():
    """Convenience function to get the Photoshop app.

    This could later query whether the platform is Windows or Mac.

    This needs to be a function call because when calling Dispatch directly
    from a different thread will result in "CoInitialize has not been called"
    which can be fixed with pythoncom.CoInitialize(). However even then it will
    still error with "The application called an interface that was marshalled
    for a different thread"
    """
    return Dispatch("Photoshop.Application")


def safe_excepthook(*args):
    traceback.print_exception(*args)


def launch(application):
    """Starts the web server that will be hosted in the Photoshop extension.
    """
    from avalon import api, photoshop

    api.install(photoshop)
    sys.excepthook = safe_excepthook

    # Launch Photoshop and the html server.
    process = subprocess.Popen(application, stdout=subprocess.PIPE)
    server = html_server.app.start_server(5000)

    while True:
        if process.poll() is not None:
            print("Photoshop process is not alive. Exiting")
            server.shutdown()
            sys.exit(1)
        try:
            _app = photoshop.app()
            if _app:
                break
        except Exception:
            time.sleep(0.1)

    # Wait for application launch to show Workfiles.
    if os.environ.get("AVALON_PHOTOSHOP_WORKFILES_ON_LAUNCH", False):
        # Wait for Photoshop launch.
        if photoshop.app():
            workfiles.show(save=False)

    # Wait for Photoshop launch.
    if photoshop.app():
        api.emit("application.launched")

    self.callback_queue = queue.Queue()
    while True:
        main_thread_listen()

    # Wait on Photoshop to close before closing the html server.
    process.wait()
    server.shutdown()


def imprint(layer, data):
    """Write `data` to the active document "headline" field as json.

    Arguments:
        layer (win32com.client.CDispatch): COMObject of the layer.
        data (dict): Dictionary of key/value pairs.

    Example:
        >>> from avalon.photoshop import lib
        >>> layer = app.ActiveDocument.ArtLayers.Add()
        >>> data = {"str": "someting", "int": 1, "float": 0.32, "bool": True}
        >>> lib.imprint(layer, data)
    """
    _app = app()

    layers_data = {}
    try:
        layers_data = json.loads(_app.ActiveDocument.Info.Headline)
    except json.decoder.JSONDecodeError:
        pass

    # json.dumps writes integer values in a dictionary to string, so
    # anticipating it here.
    if str(layer.id) in layers_data:
        layers_data[str(layer.id)].update(data)
    else:
        layers_data[str(layer.id)] = data

    # Ensure only valid ids are stored.
    layer_ids = []
    for layer in get_layers_in_document():
        layer_ids.append(layer.id)

    cleaned_data = {}
    for id in layers_data:
        if int(id) in layer_ids:
            cleaned_data[id] = layers_data[id]

    # Write date to FileInfo headline.
    _app.ActiveDocument.Info.Headline = json.dumps(cleaned_data, indent=4)


def read(layer):
    """Read the layer metadata in to a dict

    Args:
        layer (win32com.client.CDispatch): COMObject of the layer.

    Returns:
        dict
    """
    layers_data = {}
    try:
        layers_data = json.loads(app().ActiveDocument.Info.Headline)
    except json.decoder.JSONDecodeError:
        pass

    return layers_data.get(str(layer.id))


@contextlib.contextmanager
def maintained_selection():
    """Maintain selection during context."""
    selection = get_selected_layers()
    try:
        yield selection
    finally:
        select_layers(selection)


@contextlib.contextmanager
def maintained_visibility():
    """Maintain visibility during context."""
    visibility = {}
    layers = get_layers_in_document(app().ActiveDocument)
    for layer in layers:
        visibility[layer.id] = layer.Visible
    try:
        yield
    finally:
        for layer in layers:
            layer.Visible = visibility[layer.id]


def group_selected_layers():
    """Create a group and adds the selected layers.

    Returns:
        LayerSet: Created group.
    """

    _app = app()

    ref = Dispatch("Photoshop.ActionReference")
    ref.PutClass(_app.StringIDToTypeID("layerSection"))

    lref = Dispatch("Photoshop.ActionReference")
    lref.PutEnumerated(
        _app.CharIDToTypeID("Lyr "),
        _app.CharIDToTypeID("Ordn"),
        _app.CharIDToTypeID("Trgt")
    )

    desc = Dispatch("Photoshop.ActionDescriptor")
    desc.PutReference(_app.CharIDToTypeID("null"), ref)
    desc.PutReference(_app.CharIDToTypeID("From"), lref)

    _app.ExecuteAction(
        _app.CharIDToTypeID("Mk  "),
        desc,
        get_com_objects().constants().psDisplayNoDialogs
    )

    return _app.ActiveDocument.ActiveLayer


def get_selected_layers():
    """Get the selected layers

    Returns:
        list
    """
    _app = app()

    group_selected_layers()

    selection = list(_app.ActiveDocument.ActiveLayer.Layers)

    _app.ExecuteAction(
        _app.CharIDToTypeID("undo"),
        None,
        get_com_objects().constants().psDisplayNoDialogs
    )

    return selection


def get_layers_by_ids(ids):
    return [x for x in app().ActiveDocument.Layers if x.id in ids]


def select_layers(layers):
    """Selects multiple layers

    Args:
        layers (list): List of COMObjects.
    """
    _app = app()

    ref = Dispatch("Photoshop.ActionReference")
    for id in [x.id for x in layers]:
        ref.PutIdentifier(_app.CharIDToTypeID("Lyr "), id)

    desc = Dispatch("Photoshop.ActionDescriptor")
    desc.PutReference(_app.CharIDToTypeID("null"), ref)
    desc.PutBoolean(_app.CharIDToTypeID("MkVs"), False)

    try:
        _app.ExecuteAction(
            _app.CharIDToTypeID("slct"),
            desc,
            get_com_objects().constants().psDisplayNoDialogs
        )
    except Exception:
        pass


def _recurse_layers(layers):
    """Recursively get layers in provided layers.

    Args:
        layers (list): List of COMObjects.

    Returns:
        List of COMObjects.
    """
    result = {}
    for layer in layers:
        result[layer.id] = layer
        if layer.LayerType == get_com_objects().constants().psLayerSet:
            result.update(_recurse_layers(list(layer.Layers)))

    return result


def get_layers_in_layers(layers):
    """Get all layers in layers.

    Args:
        layers (list of COMObjects): layers to get layers within. Typically
            LayerSets.

    Return:
        list: Top-down recursive list of layers.
    """
    return list(_recurse_layers(layers).values())


def get_layers_in_document(document=None):
    """Get all layers in a document.

    Args:
        document (win32com.client.CDispatch): COMObject of the document. If
            None is supplied the ActiveDocument is used.

    Return:
        list: Top-down recursive list of layers.
    """
    document = document or app().ActiveDocument
    return list(_recurse_layers(list(x for x in document.Layers)).values())


def import_smart_object(path):
    """Import the file at `path` as a smart object to active document.

    Args:
        path (str): File path to import.

    Return:
        COMObject: Smart object layer.
    """
    _app = app()

    desc1 = Dispatch("Photoshop.ActionDescriptor")
    desc1.PutPath(_app.CharIDToTypeID("null"), path)
    desc1.PutEnumerated(
        _app.CharIDToTypeID("FTcs"),
        _app.CharIDToTypeID("QCSt"),
        _app.CharIDToTypeID("Qcsa")
    )

    desc2 = Dispatch("Photoshop.ActionDescriptor")
    desc2.PutUnitDouble(
        _app.CharIDToTypeID("Hrzn"), _app.CharIDToTypeID("#Pxl"), 0.0
    )
    desc2.PutUnitDouble(
        _app.CharIDToTypeID("Vrtc"), _app.CharIDToTypeID("#Pxl"), 0.0
    )

    desc1.PutObject(
        _app.CharIDToTypeID("Ofst"), _app.CharIDToTypeID("Ofst"), desc2
    )

    _app.ExecuteAction(
        _app.CharIDToTypeID("Plc "),
        desc1,
        get_com_objects().constants().psDisplayNoDialogs
    )
    layer = get_selected_layers()[0]
    layer.MoveToBeginning(_app.ActiveDocument)

    return layer


def replace_smart_object(layer, path):
    """Replace the smart object `layer` with file at `path`

    Args:
        layer (win32com.client.CDispatch): COMObject of the layer.
        path (str): File to import.
    """
    _app = app()

    _app.ActiveDocument.ActiveLayer = layer

    desc = Dispatch("Photoshop.ActionDescriptor")
    desc.PutPath(_app.CharIDToTypeID("null"), path.replace("\\", "/"))
    desc.PutInteger(_app.CharIDToTypeID("PgNm"), 1)

    _app.ExecuteAction(
        _app.StringIDToTypeID("placedLayerReplaceContents"),
        desc,
        get_com_objects().constants().psDisplayNoDialogs
    )
