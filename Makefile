all:
	$(MAKE) -C core
	$(MAKE) -C harness

clean:
	$(MAKE) -C core clean
	$(MAKE) -C harness clean

.PHONY: all clean
