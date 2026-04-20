CXX      = g++
CXXFLAGS = -O2 -Wall -std=c++11
LDFLAGS  = -lncursesw -lpthread

TARGET  = tcfplay
VERSION = 2026.4.2
ARCH    = $(shell dpkg --print-architecture)
DEBDIR  = $(TARGET)_$(VERSION)_$(ARCH)

$(TARGET): tcfplay.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) tcfplay.cpp $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

deb: $(TARGET)
	mkdir -p $(DEBDIR)/DEBIAN
	mkdir -p $(DEBDIR)/usr/local/bin
	mkdir -p $(DEBDIR)/usr/share/doc/$(TARGET)
	mkdir -p $(DEBDIR)/usr/share/man/man1

	install -m755 $(TARGET) $(DEBDIR)/usr/local/bin/$(TARGET)

	printf 'Package: $(TARGET)\nVersion: $(VERSION)\nArchitecture: $(ARCH)\n' > $(DEBDIR)/DEBIAN/control
	printf 'Maintainer: Your Name <you@example.com>\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Depends: ffmpeg, libncursesw6\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Recommends: fonts-noto-cjk\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Section: sound\nPriority: optional\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Homepage: https://github.com/yourusername/$(TARGET)\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Description: Terminal media player with spectrum visualizer\n' >> $(DEBDIR)/DEBIAN/control
	printf ' tcfplay (The Chapter Free Player) plays audio from any media file\n' >> $(DEBDIR)/DEBIAN/control
	printf ' in the terminal. Supports chapters, playlist, video mode, and a\n' >> $(DEBDIR)/DEBIAN/control
	printf ' real-time spectrum visualizer for chapter-free files.\n' >> $(DEBDIR)/DEBIAN/control

	cp LICENSE $(DEBDIR)/usr/share/doc/$(TARGET)/copyright 2>/dev/null || \
	  printf 'MIT License\n' > $(DEBDIR)/usr/share/doc/$(TARGET)/copyright

	printf '.TH TCFPLAY 1 "2026" "tcfplay $(VERSION)" "User Commands"\n.SH NAME\ntcfplay \\- terminal media player\n.SH SYNOPSIS\n.B tcfplay\n[options]\n.I file ...\n.SH OPTIONS\n.TP\n.B -help\nShow help\n.TP\n.B -version\nShow version\n.TP\n.B -video\nEnable video window\n.TP\n.B -playlist\nPlay all files in sequence\n.SH CONTROLS\n.TP\n.B Space\nPause/resume\n.TP\n.B Left/Right\nSeek 10s\n.TP\n.B Up/Down\nVolume\n.TP\n.B s/w\nChapter next/prev\n.TP\n.B n\nNext in playlist\n.TP\n.B q\nQuit\n' \
	  | gzip -9 > $(DEBDIR)/usr/share/man/man1/tcfplay.1.gz

	dpkg-deb --build --root-owner-group $(DEBDIR)
	rm -rf $(DEBDIR)
	@echo "Built: $(DEBDIR).deb"

clean:
	rm -f $(TARGET)
	rm -rf $(DEBDIR) $(DEBDIR).deb

.PHONY: install deb clean
