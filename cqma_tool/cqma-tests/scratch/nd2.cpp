int main(int argc, char**) {
    int* p = nullptr;
    if (argc > 100) p = new int(3);
    int v = *p;
    delete p;
    return v;
}
