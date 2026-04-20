CXX      = g++
CXXFLAGS = -O2 -Wall -std=c++11
LDFLAGS  = -lncursesw -lpthread

TARGET  = tcfplay
VERSION = 2026.4
ARCH    = $(shell dpkg --print-architecture)
DEBDIR  = $(TARGET)_$(VERSION)_$(ARCH)

$(TARGET): tcfplay.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) tcfplay.cpp $(LDFLAGS)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

# Build a .deb package
# Usage: make deb
# Output: tcfplay_1.0.0_amd64.deb
deb: $(TARGET)
	mkdir -p $(DEBDIR)/DEBIAN
	mkdir -p $(DEBDIR)/usr/local/bin
	mkdir -p $(DEBDIR)/usr/share/doc/$(TARGET)
	mkdir -p $(DEBDIR)/usr/share/man/man1

	install -m755 $(TARGET) $(DEBDIR)/usr/local/bin/$(TARGET)

	printf 'Package: $(TARGET)\nVersion: $(VERSION)\nArchitecture: $(ARCH)\n' > $(DEBDIR)/DEBIAN/control
	printf 'Maintainer: ernugraha <ernugraha24@gmail.com>\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Depends: ffmpeg, libncursesw6\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Recommends: fonts-noto-cjk\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Section: sound\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Priority: optional\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Homepage: https://github.com/ernugraha/$(TARGET)\n' >> $(DEBDIR)/DEBIAN/control
	printf 'Description: Terminal audio/video player with spectrum visualizer\n' >> $(DEBDIR)/DEBIAN/control
	printf ' tcfplay plays audio from any media file in the terminal using ffplay.\n' >> $(DEBDIR)/DEBIAN/control
	printf ' Video is suppressed even for MP4/MKV. Files with chapters show a chapter\n' >> $(DEBDIR)/DEBIAN/control
	printf ' list; files without show a real-time spectrum visualizer.\n' >> $(DEBDIR)/DEBIAN/control
	printf ' Designed for low-end hardware (2011 PCs and up).\n' >> $(DEBDIR)/DEBIAN/control

	cp LICENSE $(DEBDIR)/usr/share/doc/$(TARGET)/copyright 2>/dev/null || \
	  printf 'MIT License\n' > $(DEBDIR)/usr/share/doc/$(TARGET)/copyright

	printf '.TH TCFPLAY 1 "2025" "tcfplay $(VERSION)" "User Commands"\n.SH NAME\ntcfplay \\- terminal audio player\n.SH SYNOPSIS\n.B tcfplay\n.I FILE\n.SH CONTROLS\n.TP\n.B Space\nPause/resume\n.TP\n.B Left/Right\nSeek 10s\n.TP\n.B Up/Down\nVolume\n.TP\n.B s/w\nChapter next/prev\n.TP\n.B q\nQuit\n' \
	  | gzip -9 > $(DEBDIR)/usr/share/man/man1/tcfplay.1.gz

	dpkg-deb --build --root-owner-group $(DEBDIR)
	rm -rf $(DEBDIR)
	@echo "Built: $(DEBDIR).deb"

clean:
	rm -f $(TARGET)
	rm -rf $(DEBDIR) $(DEBDIR).deb

.PHONY: install deb clean
