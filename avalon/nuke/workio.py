"""Host API required Work Files tool"""
import os
import nuke
from avalon import api


def file_extensions():
    return api.HOST_WORKFILE_EXTENSIONS["nuke"]


def has_unsaved_changes():
    return nuke.root().modified()


def save_file(filepath):
    path = filepath.replace("\\", "/")
    nuke.scriptSaveAs(path)
    nuke.Root()["name"].setValue(path)
    nuke.Root()["project_directory"].setValue(os.path.dirname(path))
    nuke.Root().setModified(False)


def open_file(filepath):
    filepath = filepath.replace("\\", "/")

    # To remain in the same window, we have to clear the script and read
    # in the contents of the workfile.
    nuke.scriptClear()
    nuke.scriptReadFile(filepath)
    nuke.Root()["name"].setValue(filepath)
    nuke.Root()["project_directory"].setValue(os.path.dirname(filepath))
    nuke.Root().setModified(False)

    # Since we clear the current script and read in contents of the file path
    # instead of loading the script (to stay within the same window), there are
    # no callbacks emitted by Nuke. To accommodate callbacks on loading we
    # introduce this signal.
    api.emit("workio.open_file")

    return True


def current_file():
    current_file = nuke.root().name()

    # Unsaved current file
    if current_file == 'Root':
        return None

    return os.path.normpath(current_file).replace("\\", "/")


def work_root(session):

    work_dir = session["AVALON_WORKDIR"]
    scene_dir = session.get("AVALON_SCENEDIR")
    if scene_dir:
        path = os.path.join(work_dir, scene_dir)
    else:
        path = work_dir

    return os.path.normpath(path).replace("\\", "/")
