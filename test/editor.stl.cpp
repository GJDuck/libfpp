// g++ editor.stl.cpp -lncurses -std=c++17 -O2

#include <ncurses.h>
#include <string>
#include <vector>

using namespace std;

struct Editor {
  struct Cursor { string buf; size_t pos = 0; };
  Cursor cursor;
  struct { vector<Cursor> undo, redo; } history;
  void commit() {
   history.undo.push_back(cursor);
   history.redo.clear();
  }
  void insert(char c) {
   commit();
   cursor.buf.insert(cursor.pos, 1, c);
   cursor.pos++;
  }
  void backspace() {
   if (cursor.pos == 0) return;
   commit();
   cursor.pos--;
   cursor.buf.erase(cursor.pos);
  }
  void left() {
   if (cursor.pos > 0) cursor.pos--;
  }
  void right() {
   if (cursor.pos < cursor.buf.size()) cursor.pos++;
  }
  void undo() {
   if (history.undo.empty()) return;
   history.redo.push_back(cursor);
   cursor = std::move(history.undo.back());
   history.undo.pop_back();
  }
  void redo() {
    if (history.redo.empty()) return;
    history.undo.push_back(cursor);
    cursor = std::move(history.redo.back());
    history.redo.pop_back();
  }
};

void draw(Editor &ed)
{
    clear();
    mvprintw(0,0,"Naive STL Editor (ESC to quit, Ctrl-Z undo, Ctrl-Y redo)");
    mvprintw(2,0,"%s", ed.cursor.buf.c_str());
    move(2, ed.cursor.pos);
    refresh();
}

int main()
{
    Editor ed;

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
