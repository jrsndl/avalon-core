from ...vendor.Qt import QtWidgets, QtCore, QtGui, QtSvg
from ...vendor import qtawesome
from ..widgets import OptionalMenu, OptionalAction, OptionDialog
import inspect


def is_representation_loader(loader):
    return hasattr(loader, "add_site_to_representation")


def get_selected_items(rows, item_role):
    items = []
    for row_index in rows:
        item = row_index.data(item_role)
        if item.get("isGroup"):
            continue

        elif item.get("isMerged"):
            for idx in range(row_index.model().rowCount(row_index)):
                child_index = row_index.child(idx, 0)
                item = child_index.data(item_role)
                if item not in items:
                    items.append(item)

        else:
            if item not in items:
                items.append(item)
    return items


def get_options(action, loader, parent):
    # Pop option dialog
    options = {}
    if getattr(action, "optioned", False):
        dialog = OptionDialog(parent)
        dialog.setWindowTitle(action.label + " Options")
        dialog.create(loader.options)

        if not dialog.exec_():
            return

        # Get option
        options = dialog.parse()

    return options


def add_representation_loaders_to_menu(loaders, menu, optional_labels=None):
    """
        Loops through provider loaders and adds them to 'menu'.

        Expects loaders sorted in requested order.
        Expects loaders de-duplicated if wanted.

        Args:
            loaders(tuple): representation - loader
            menu (OptionalMenu):

        Returns:
            menu (OptionalMenu): with new items
    """
    # List the available loaders
    for representation, loader in loaders:
        label = None
        if optional_labels:
            label = optional_labels.get(loader)

        if not label:
            label = get_label_from_loader(loader, representation)

        icon = get_icon_from_loader(loader)

        # Optional action
        use_option = hasattr(loader, "options")
        action = OptionalAction(label, icon, use_option, menu)
        if use_option:
            # Add option box tip
            action.set_option_tip(loader.options)

        action.setData((representation, loader))

        # Add tooltip and statustip from Loader docstring
        tip = inspect.getdoc(loader)
        if tip:
            action.setToolTip(tip)
            action.setStatusTip(tip)

        menu.addAction(action)

    return menu


def remove_tool_name_from_loaders(available_loaders, tool_name):
    for loader in available_loaders:
        if hasattr(loader, "tool_names"):
            if not (
                    "*" in loader.tool_names or
                    tool_name in loader.tool_names
            ):
                available_loaders.remove(loader)
    return available_loaders


def get_icon_from_loader(loader):
    """Pull icon info from loader class"""
    # Support font-awesome icons using the `.icon` and `.color`
    # attributes on plug-ins.
    icon = getattr(loader, "icon", None)
    if icon is not None:
        try:
            key = "fa.{0}".format(icon)
            color = getattr(loader, "color", "white")
            icon = qtawesome.icon(key, color=color)
        except Exception as e:
            print("Unable to set icon for loader "
                  "{}: {}".format(loader, e))
            icon = None
    return icon


def get_label_from_loader(loader, representation=None):
    """Pull label info from loader class"""
    label = getattr(loader, "label", None)
    if label is None:
        label = loader.__name__
    if representation:
        # Add the representation as suffix
        label = "{0} ({1})".format(label, representation['name'])
    return label


def get_no_loader_action(menu, one_item_selected=False):
    """Creates dummy no loader option in 'menu'"""
    submsg = "your selection."
    if one_item_selected:
        submsg = "this version."
    msg = "No compatible loaders for {}".format(submsg)
    print(msg)
    icon = qtawesome.icon(
        "fa.exclamation",
        color=QtGui.QColor(255, 51, 0)
    )
    action = OptionalAction(("*" + msg), icon, False, menu)
    return action
