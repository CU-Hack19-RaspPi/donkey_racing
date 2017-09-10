#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <list>
#include <string>
#include <vector>
#include <map>

extern "C" {

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libavutil/buffer.h>
#include <libavutil/log.h>

}

#include "math2.h"
#include "../pilot/image.h"

#include <caffe2/db/create_db_op.h>


bool verbose = false;
bool progress = isatty(2);
uint64_t maxconcatoffset;


struct file_header {
    char type[4];
    uint32_t zero;
    char subtype[4];
};

struct chunk_header {
    char type[4];
    uint32_t size;
};

struct stream_chunk {
    FILE *file;
    uint64_t concatoffset;
    uint32_t fileoffset;
    uint32_t streamoffset;
    uint32_t size;
};

std::list<stream_chunk> h264Chunks;
std::list<stream_chunk> timeChunks;
std::list<stream_chunk> pdtsChunks;
std::map<FILE *, std::string> fileNames;

struct steer_packet {
    uint16_t code;
    int16_t steer;
    int16_t throttle;
};
struct throttle_steer {
    float throttle;
    float steer;
};
struct pts_dts {
    uint64_t pts;
    uint64_t dts;
};

struct dump_chunk {
    char type[4];
    std::string filename;
};

std::list<dump_chunk> dumpChunks;
std::list<std::string> filenameArgs;
std::string datasetName;

void check_header(FILE *f, char const *name) {
    file_header fh;
    if ((12 != fread(&fh, 1, 12, f)) || strncmp(fh.type, "RIFF", 4) || strncmp(fh.subtype, "h264", 4)) {
        fprintf(stderr, "%s: not a h264 RIFF file\n", name);
        exit(1);
    }
}

bool generate_requested_file(char const *type, char const *output) {
    abort();
    return false;
}

char const *get_filename(FILE *f) {
    static char ret[32];
    auto ptr = fileNames.find(f);
    if (ptr == fileNames.end()) {
        sprintf(ret, "%30s", "???");
    } else {
        char const *p = (*ptr).second.c_str();
        if (strlen(p) > 30) {
            p += strlen(p)-30;
        }
        sprintf(ret, "%30s", p);
    }
    return ret;
}

bool generate_dataset(char const *output) {
    if (verbose) {
        av_log_set_level(99);
    }
    avcodec_register_all();
    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "avcodec_find_decoder(): h264 not found\n");
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        fprintf(stderr, "avcodec_alloc_context3(): failed to allocate\n");
        return false;
    }
    ctx->flags2 |= AV_CODEC_FLAG2_CHUNKS;
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2(): failed to open\n");
        return false;
    }
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "av_frame_alloc(): alloc failed\n");
        return false;
    }
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    if (!parser) {
        fprintf(stderr, "av_parser_init(): h264 failed\n");
        return false;
    }
    AVPacket avp = { 0 };
    av_init_packet(&avp);

    //  loop
    auto curpdts = pdtsChunks.begin(), endpdts = pdtsChunks.end();
    auto curtime = timeChunks.begin(), endtime = timeChunks.end();
    auto curh264 = h264Chunks.begin(), endh264 = h264Chunks.end();
    throttle_steer ts = { 0 };
    pts_dts pd = { 0 };
    int frameno = 0;
    uint64_t ptsbase = 0;
    uint64_t dtsbase = 0;
    std::vector<unsigned char> readBuf;
    std::vector<unsigned char> tsbuf;
    uint64_t timeStart = 0;
    int nprogress = 1;
    while (curpdts != endpdts && curtime != endtime && curh264 != endh264) {
        while (curtime != endtime &&
                curtime->concatoffset < curpdts->concatoffset) {
            if (curtime->size >= 6) {
                fseek(curtime->file, curtime->fileoffset, 0);
                tsbuf.resize(curtime->size);
                fread(&tsbuf[0], 1, curtime->size, curtime->file);
                steer_packet sp = { 0 };
                uint64_t timeNow;
                memcpy(&timeNow, &tsbuf[0], 8);
                if (!timeStart) {
                    timeStart = timeNow;
                }
                for (size_t offset = 8; offset <= curtime->size-6;) {
                    memcpy(&sp, &tsbuf[offset], 6);
                    if (sp.code == 'S') {
                        //  steercontrol
                        ts.throttle = sp.throttle / 16383.0f;
                        ts.steer = sp.steer / 16383.0f;
                        if (verbose) {
                            fprintf(stderr, "now=%.3f throttle=%.2f steer=%.2f\n",
                                    (timeNow-timeStart) * 1e-6, ts.throttle, ts.steer);
                        }
                        offset += 6;
                    } else if (sp.code == 'T') {
                        //  triminfo
                        offset += 10;
                    } else if (sp.code == 'i') {
                        //  ibus
                        offset += 22;
                    } else {
                        if (verbose) {
                            fprintf(stderr, "unknown metadata type 0x%04x\n", sp.code);
                        }
                        break;
                    }
                }
            }
            ++curtime;
        }
        while (curpdts != endpdts &&
                curpdts->concatoffset < curh264->concatoffset) {
            fseek(curpdts->file, curpdts->fileoffset, 0);
            fread(&pd, 1, 16, curpdts->file);
            ++curpdts;
            if (frameno == 0) {
                ptsbase = pd.pts;
                dtsbase = pd.dts;
            }
        }
        while (curh264 != endh264 &&
                curh264->concatoffset < curtime->concatoffset &&
                curh264->concatoffset < curpdts->concatoffset) {
            fseek(curh264->file, curh264->fileoffset, 0);
            size_t readBufOffset = readBuf.size();
            readBuf.resize(readBufOffset + curh264->size);
            fread(&readBuf[readBufOffset], 1, curh264->size, curh264->file);
            avp.pos = curh264->streamoffset;
parse_more:
            avp.data = NULL;
            avp.size = 0;
            avp.pts = pd.pts - ptsbase;
            avp.dts = pd.dts ? pd.dts - dtsbase : pd.pts - ptsbase;
            int lenParsed = av_parser_parse2(parser, ctx, &avp.data, &avp.size, 
                    &readBuf[0], readBuf.size(), avp.pts, avp.dts, avp.pos);
            if (verbose) {
                fprintf(stderr, "av_parser_parse2(): lenParsed %d size %d pointer %p readbuf 0x%p\n",
                        lenParsed, avp.size, avp.data, &readBuf[0]);
            }
            if (avp.size) {
                int lenSent = avcodec_send_packet(ctx, &avp);
                if (lenSent < 0) {
                    if (verbose) {
                        fprintf(stderr, "avcodec_send_packet(): error %d at concatoffset %ld\n",
                                lenSent, avp.pos);
                    }
                }
                readBuf.erase(readBuf.begin(), readBuf.begin()+lenParsed);
                int err = avcodec_receive_frame(ctx, frame);
                if (err == 0) {
                    //  got a frame!
                    ++frameno;
                    if (verbose) {
                        fprintf(stderr, "frame %d concatoffset %ld pts %ld steer %.2f drive %.2f format %d size %dx%d ptrs %p@%d %p@%d %p@%d\n",
                                frameno, curh264->concatoffset, pd.pts, ts.steer, ts.throttle,
                                frame->format, frame->width, frame->height,
                                frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1],
                                frame->data[2], frame->linesize[2]);
                    }
                    if (ctx->refcounted_frames) {
                        av_frame_unref(frame);
                    }
                } else if (err == AVERROR(EAGAIN)) {
                    //  nothing for now
                } else if (err == AVERROR_EOF) {
                    //  nothing for now
                } else {
                    //  not a header
                    if (curh264->size > 128) {
                        if (verbose) {
                            fprintf(stderr, "avcodec_receive_frame() error %d concatoffset %ld\n", 
                                    err, curh264->concatoffset);
                        }
                        // return false;
                    }
                }
                avp.pos += avp.size;
                goto parse_more;
            }
            readBuf.erase(readBuf.begin(), readBuf.begin()+lenParsed);
            ++curh264;
        }

        if (progress) {
            if (!--nprogress) {
                double fraction = double(curh264->concatoffset) / maxconcatoffset;
                fprintf(stderr, "%.30s: [%s%s] %6.2f%%\r", 
                        get_filename(curh264->file),
                        "==================================>" + int((1-fraction)*35),
                        "                                   " + int(fraction*35),
                        fraction*100);
                nprogress = 20;
            }
        }
    }

    if (progress) {
        fprintf(stderr, "\n");
    }

    //  todo cleanup
    return true;
}

void parse_arguments(int argc, char const *argv[]) {

    if (argc == 1 || argv[0][0] == '-') {
usage:
        fprintf(stderr, "usage: mktrain [options] file1.riff file2.riff ...\n");
        fprintf(stderr, "--dump type:filename\n");
        fprintf(stderr, "--dataset name.lmdb\n");
        fprintf(stderr, "--verbose\n");
        fprintf(stderr, "--quiet\n");
        exit(1);
    }

    for (int i = 1; i != argc; ++i) {

        if (argv[i][0] == '-') {

            if (!strcmp(argv[i], "--dataset")) {
                ++i;
                if (!argv[i]) {
                    fprintf(stderr, "--dataset requires filename.lmdb\n");
                    exit(1);
                }
                char const *dot = strrchr(argv[i], '.');
                if (dot && !strcmp(dot, ".riff")) {
                    //  Is this an existing .riff file? Likely a typo, forgetting 
                    //  to name the output dataset.
                    struct stat stbuf;
                    if (!stat(argv[i], &stbuf)) {
                        fprintf(stderr, "refusing to use an existing .riff file (%s) as output dataset\n",
                                argv[i]);
                        exit(1);
                    }
                }
                datasetName = argv[i];
                continue;
            }

            if (!strcmp(argv[i], "--dump")) {
                ++i;
                if (!argv[i]) {
                    fprintf(stderr, "--dump requires type:filename\n");
                    exit(1);
                }
                if (strlen(argv[i]) < 6 || argv[i][4] != ':') {
                    fprintf(stderr, "--dump bad argument: %s\n", argv[i]);
                    exit(1);
                }
                dump_chunk dc;
                memcpy(dc.type, argv[i], 4);
                dc.filename = &argv[i][5];
                dumpChunks.push_back(dc);
                continue;
            }

            if (!strcmp(argv[i], "--verbose")) {
                verbose = true;
                progress = false;
                continue;
            }

            if (!strcmp(argv[i], "--quiet")) {
                verbose = false;
                progress = false;
                continue;
            }

            fprintf(stderr, "unknown argument: '%s'\n", argv[i]);
            goto usage;
        } else {
            filenameArgs.push_back(argv[i]);
        }
    }

    if (!filenameArgs.size()) {
        fprintf(stderr, "no input files specified\n");
        goto usage;
    }
}

FILE *open_check(char const *cpath, long *len) {

    FILE *f = fopen(cpath, "rb");
    if (!f) {
        perror(cpath);
        exit(1);
    }

    check_header(f, cpath);

    fseek(f, 0, 2);
    *len = ftell(f);
    fseek(f, 12, 0);

    if (verbose) {
        fprintf(stdout, "%s: %ld bytes\n", cpath, *len);
    }
    fileNames[f] = cpath;

    return f;
}


void slurp_files() {

    uint64_t h264Offset = 0;
    uint64_t pdtsOffset = 0;
    uint64_t timeOffset = 0;
    uint64_t concatoffset = 0;

    for (auto const &path : filenameArgs) {

        char const *cpath = path.c_str();
        long len = 0;

        if (progress) {
            char pathstr[50];
            char const *q = cpath;
            if (strlen(q) > 49) {
                q += strlen(q)-49;
            }
            sprintf(pathstr, "%.49s", q);
            fprintf(stderr, "reading: %49s\r", pathstr);
        }

        FILE *f = open_check(cpath, &len);

        while (!feof(f) && !ferror(f)) {

            long pos = ftell(f);

            chunk_header ch;
            long rd = fread(&ch, 1, 8, f); 
            if (8 != rd) {
                if (rd != 0) {
                    perror("short read");
                }
                break;
            }

            stream_chunk sc;
            sc.file = f;
            sc.concatoffset = pos + concatoffset;
            sc.fileoffset = pos + 8;
            sc.streamoffset = 0;
            sc.size = ch.size;

            if (!strncmp(ch.type, "h264", 4)) {
                sc.streamoffset = h264Offset;
                h264Offset += sc.size;
                h264Chunks.push_back(sc);
            }
            else if (!strncmp(ch.type, "pdts", 4)) {
                sc.streamoffset = pdtsOffset;
                pdtsOffset += sc.size;
                pdtsChunks.push_back(sc);
            }
            else if (!strncmp(ch.type, "time", 4)) {
                sc.streamoffset = timeOffset;
                timeOffset += sc.size;
                timeChunks.push_back(sc);
            }

            fseek(f, (ch.size + 3) & ~3, 1);
        }

        concatoffset += ftell(f);
    }

    if (progress) {
        fprintf(stderr, "\n");
    }

    if (verbose) {
        fprintf(stdout, "%ld h264 chunks size %ld\n%ld pdts chunks size %ld\n%ld time chunks size %ld\n",
                h264Chunks.size(), h264Offset, pdtsChunks.size(), pdtsOffset, timeChunks.size(), timeOffset);
        fprintf(stdout, "total size %ld bytes over %ld files\n", concatoffset, filenameArgs.size());
    }

    maxconcatoffset = concatoffset;
}


int main(int argc, char const *argv[]) {

    struct timeval starttime = { 0 };
    gettimeofday(&starttime, 0);

    parse_arguments(argc, argv);

    slurp_files();
   
    int errors = 0;
    for (auto const &dc : dumpChunks) {
        if (!generate_requested_file(dc.type, dc.filename.c_str())) {
            fprintf(stderr, "Could not generate '%s' of type '%.4s'\n", dc.filename.c_str(), dc.type);
            ++errors;
        }
    }
    if (datasetName.size()) {
        if (!generate_dataset(datasetName.c_str())) {
            fprintf(stderr, "Error generting dataset '%s'\n", datasetName.c_str());
            ++errors;
        }
    }

    struct timeval endtime = { 0 };
    gettimeofday(&endtime, 0);

    double seconds = (endtime.tv_sec - starttime.tv_sec) + 
        (endtime.tv_usec - starttime.tv_usec) * 1e-6;
    if (progress || verbose) {
        fprintf(stderr, "%s in %dh %dm %.1fs\n",
                errors ? "errorored" : "completed",
                int(seconds)/3600, int(seconds)/60%60, fmod(seconds, 60));
    }

    return errors;
}
