CPPFLAGS += -I../../../libmpv/
CFLAGS += -fPIC
LDFLAGS += -shared -fPIC

example.so: example.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(LIBS) -o $@ $<

clean:
	$(RM) example.so
