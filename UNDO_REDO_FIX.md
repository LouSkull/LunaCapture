# Undo / Redo fix

The region editor now has a real two-stack history.

- Undo button uses a classic curved left arrow.
- Redo button uses a matching curved right arrow.
- Ctrl+Z: Undo.
- Ctrl+Y or Ctrl+Shift+Z: Redo.
- Disabled history buttons are dimmed and not clickable.
- A new drawing after Undo clears the Redo branch.
- Visible product version remains 1.0.0.0.
