int main() { int *p = new int(7); int result = *p != 7; delete p; return result; }
