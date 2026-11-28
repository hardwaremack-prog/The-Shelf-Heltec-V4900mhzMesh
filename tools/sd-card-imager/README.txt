SD Card Imager
==============

Saves a whole SD card to one image file, and writes an image back onto a card.
Use it to back up a card, or to copy one card to another (save the old card,
then write the image onto the new one). Windows 10 or 11, nothing to install.

Start it
  Double-click "SD Card Imager.cmd" and click Yes when Windows asks about
  admin rights (Windows only lets admin programs read or write a whole card).
  Keep both files in the same folder.

Save a card
  1. Put the card in and pick it in the Card list (click Refresh if it's missing).
  2. Leave "Compress" ticked: empty space on the card takes almost nothing,
     so a 32 GB card with 3 GB on it makes an image of roughly 3 GB.
  3. Click "Save card to image..." and choose where to save it.
  With "Quick" ticked (the default), a FAT32 card is read only where it holds
  something: the app reads the card's own map of used space and skips the rest,
  so a 32 GB card with 3 GB on it takes a few minutes. The image is still a
  normal full-size card image (empty space is zeros). Untick Quick to read
  every byte, or for cards that aren't FAT32 (those are always read whole).

Write an image onto a card
  1. Put the new card in and pick it in the Card list. CHECK IT'S THE RIGHT ONE:
     everything on it is replaced.
  2. Click "Write image to card..." and pick the .img or .img.gz file.
  3. When it's done, take the card out and put it back in.
  With Quick ticked, empty space is skipped on the new card too.
  The new card must be at least as big as the one the image came from.
  A bigger card works: when the write finishes, the app offers to grow the
  card so the extra space gets used (see below).

Grow a card to its full size
  After writing a 32 GB image onto a 128 GB card, the card still looks like
  32 GB. Pick the card and click "Grow to fill card..." (the app also offers
  this by itself right after a write).
  - FAT32 cards: the partition and the file system both grow; your files stay
    where they are. Takes about a minute. Keep the image until you've checked
    the card, just in case.
  - Raspberry Pi cards: the Linux partition grows. Windows can't change the
    files inside it, so boot the Pi from the card once and run, in a Terminal:
        sudo resize2fs /dev/mmcblk0p2
    It takes a few seconds and needs no reboot. (Or use raspi-config >
    Advanced Options > Expand Filesystem.)
  - NTFS cards grow with Windows' own resize.
  exFAT and GPT cards aren't grown (the app says so and changes nothing).

Only SD cards and USB drives are listed, never the computer's own disks.
"Stop" ends a save (the unfinished file is deleted) or a write (the card is
left incomplete: write it again before using it).
