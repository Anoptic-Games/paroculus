import QtQuick
import QtQuick.Controls.Basic

// A registry-projected submenu: one MenuItem per registry row in the given
// categories, titled, bindings shown, dimmed where inapplicable. Inputs: title
// (Menu's own), categories (list of category strings). A view of the one
// registry table (App.active.actions), never a second list — so applicability
// re-dims as the selection changes and no surface holds a list that can drift.
Menu {
    id: projected
    property var categories: []

    // The registry rows in the given categories, live.
    function actionsIn(cats) {
        if (!App.active) return []
        return App.active.actions.filter(function(a) { return cats.indexOf(a.category) >= 0 })
    }

    Repeater {
        // `categories` resolves up the scope chain to the ProjectedMenu; the
        // Repeater's `parent` is the Menu's internal content view, which has
        // no such property — reading it there leaves every menu empty.
        model: projected.actionsIn(projected.categories)
        delegate: MenuItem {
            required property var modelData
            text: modelData.title + (modelData.binding.length > 0 ? "   " + modelData.binding : "")
            // A parameterized row is dimmed rather than run with no value: the
            // numeric-entry-pending flow that supplies it lands with the
            // numeric-entry widget in a later step, and a menu row that runs
            // and silently no-ops breaks the property that an applicable
            // action runs — the one that makes the whole table trustworthy.
            enabled: modelData.applicable && !modelData.needsValue
            onTriggered: App.active.run(modelData.name, {})
        }
    }
}
