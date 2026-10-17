package ui

import "testing"

func TestAppmenuCloseSubmenuWithNoItems(t *testing.T) {
	// The menu is empty whenever Tailscale is not running, and the left key
	// still calls CloseSubmenu.
	var appmenu Appmenu
	appmenu.SetItems([]*AppmenuItem{})

	appmenu.CloseSubmenu()

	if appmenu.IsSubmenuOpen() {
		t.Fatal("expected submenu to be closed")
	}
}
