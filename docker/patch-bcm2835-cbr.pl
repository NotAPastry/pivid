#!/usr/bin/perl
# Force CBR rate-control in the bcm2835-codec H.264 encoder.
#
# PiVid runs the bcm2835 HW H.264 encoder (rpicam-vid/libcamera over MMAL)
# feeding a fixed-rate LR2021 radio link: ~0.9 Mbps sustained drain behind a
# 32 KB TX ring. Under the encoder's default VBR rate control, per-frame peaks
# (the 1 s IDR burst + clustered P-frames) overflow the ring -- measured
# ~30-49% `dropped_full` on-air (2026-06-02), at every bitrate AND resolution
# tried, because the loss is burst-shaped, not average-rate. CBR flattens
# per-frame size so the stream stays within the ring/drain budget (Phase-3
# plan Gate 2).
#
# rpicam-vid/libcamera default V4L2_CID_MPEG_VIDEO_BITRATE_MODE to VBR and
# expose no CLI flag to pick CBR, so we coerce the mode in the driver. The
# single force point in bcm2835_codec_s_ctrl() suffices because
# create_component() calls v4l2_ctrl_handler_setup(), which re-applies every
# control through s_ctrl with the MMAL component live -- so the coercion fires
# on explicit S_CTRL, on the control default, and at component creation.
use strict;
use warnings;

my $path = "/opt/rpi-linux/drivers/staging/vc04_services/bcm2835-codec/bcm2835-v4l2-codec.c";

my $anchor =
    "\t\tswitch (ctrl->val) {\n" .
    "\t\tdefault:\n" .
    "\t\tcase V4L2_MPEG_VIDEO_BITRATE_MODE_VBR:\n" .
    "\t\t\tbitrate_mode = MMAL_VIDEO_RATECONTROL_VARIABLE;\n" .
    "\t\t\tbreak;\n" .
    "\t\tcase V4L2_MPEG_VIDEO_BITRATE_MODE_CBR:\n" .
    "\t\t\tbitrate_mode = MMAL_VIDEO_RATECONTROL_CONSTANT;\n" .
    "\t\t\tbreak;\n" .
    "\t\t}\n";

my $force =
    "\n" .
    "\t\t/*\n" .
    "\t\t * pivid: force CBR regardless of the requested mode. The LR2021\n" .
    "\t\t * link has a fixed ~0.9 Mbps drain behind a 32 KB TX ring; VBR\n" .
    "\t\t * peak frames overflow the ring (~30-49% dropped_full measured\n" .
    "\t\t * on-air 2026-06-02). rpicam-vid/libcamera default this control\n" .
    "\t\t * to VBR and expose no CBR flag, so coerce it here. Fires via\n" .
    "\t\t * v4l2_ctrl_handler_setup() at component creation too.\n" .
    "\t\t */\n" .
    "\t\tbitrate_mode = MMAL_VIDEO_RATECONTROL_CONSTANT;\n";

local $/;
open(my $fh, '<', $path) or die "open $path: $!";
my $src = <$fh>;
close($fh);

my $n = () = ($src =~ /\Q$anchor\E/g);
die "FATAL: expected exactly 1 bitrate_mode switch anchor, found $n (already patched, or source changed)\n"
    unless $n == 1;

$src =~ s/\Q$anchor\E/$anchor$force/;

open(my $out, '>', $path) or die "write $path: $!";
print $out $src;
close($out);
print "patched: forced CBR in bcm2835_codec_s_ctrl()\n";
