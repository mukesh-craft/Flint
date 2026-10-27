static int* maybe(bool ok) { static int v; if (ok) return &v; return nullptr; }
static int deref(bool ok) { int* p = maybe(ok); return *p; }
int main(int argc, char**) { int t = 0; if (argc > 100) t += deref(false); return t; }
