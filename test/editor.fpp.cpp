// g++ editor.fpp.cpp ../libf++.cpp -I ../ -lncurses -std=c++17 -O2

#include <ncurses.h>
#include <libf++>

using namespace F;

struct Editor {
  using Cursor = string<>::iterator;
  Cursor cursor;
  struct { vector<Cursor> undo, redo; } history;
  void commit() {
    history.undo.push_back(cursor);
    history.redo.clear();
  }
  void insert(char c) {
    commit();
    cursor.insert(c);
    
  }
  void backspace() {
    if (cursor.pos() == 0) return;
    commit();
    cursor--;
    cursor.erase();
  }
  void left() {
    if (cursor.pos() > 0) cursor--;
  }
  void right() {
    if (cursor != string<>::end()) cursor++;
  }
  void undo() {
    if (history.undo.empty()) return;
    history.redo.push_back(cursor);
    cursor = history.undo.back();
    history.undo.pop_back();
  }
  void redo() {
    if (history.redo.empty()) return;
    history.undo.push_back(cursor);
    cursor = history.redo.back();
    history.redo.pop_back();
  }
};
void draw(Editor &ed)
{
    clear();
    mvprintw(0,0,"FPP Editor Demo (ESC to quit, Ctrl-Z undo, Ctrl-Y redo)");
    mvprintw(2,0,"%s", ed.cursor.finalize().str().c_str());
    move(2, size_t(ed.cursor.pos()));
    refresh();
}

int main()
{
    Editor ed;
    string<> str;
    ed.cursor = str.begin();

    initscr();
    raw();
    keypad(stdscr, TRUE);
    noecho();

    draw(ed);

    while (true)
    {
        int ch = getch();

        if (ch == 27) // ESC
            break;
        else if (ch == KEY_LEFT)
            ed.left();
        else if (ch == KEY_RIGHT)
            ed.right();
        else if (ch == KEY_BACKSPACE || ch == 127)
            ed.backspace();
        else if (ch == 26) // Ctrl-Z
            ed.undo();
        else if (ch == 25) // Ctrl-Y
            ed.redo();
        else if (ch >= 32 && ch <= 126) // printable ASCII
            ed.insert((char)ch);
        draw(ed);
    }

    endwin();
}
