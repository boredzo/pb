#include <Carbon/Carbon.h>
#include <sys/errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "GrowlDefines.h"

/*	To-do:
 *	- Add --file in copy and paste subcmds
 *	- Add multicopy subcmd: multicopy [options] filename [options] filename …
 *	- Collapse in_fd, out_fd to one FD.
 *	- Hook up convert_encodings.
 */

struct argblock {
	int (*proc)(struct argblock *);

	const char *filename;
	int in_fd, out_fd; //Collapse this to one FD!

	//Parameters for subcommands.
	int argc;
	const char **argv;

	PasteboardRef pasteboard;
	CFStringRef pasteboardID;
	const char *pasteboardID_cstr;

	CFStringRef type; //UTI

	struct {
		unsigned reserved: 27;
		enum {
			global_options,
			subcommand,
			subcommand_options
		} phase: 2;
		unsigned has_args: 1;
		unsigned infer_translate_newlines: 1; //Fill in translate_newlines based on value of type. Default 1; set to 0 when translate_newlines is set explicitly.
		unsigned translate_newlines: 1;
	} flags;
} pb;

static void *pb_allocate  (size_t nbytes);
static void  pb_deallocate(void *buf);
static void  pb_deallocateall(void);

//Note: Any created data objects are implicitly retained (Create/Copy rule).
//Data objects passed in are not implicitly retained.
//Encodings: UTF-16, UTF-16 (with BOM), UTF-8, MacRoman
static Boolean convert_encodings(CFDataRef *inoutUTF16Data, CFDataRef *inoutUTF16ExtData, CFDataRef *inoutUTF8Data, CFDataRef *inoutMacRomanData);
//Returns a CFData containing UTF-16 (without BOM) data for the string. Counterpart to CFStringCreateExternalRepresentation.
static CFDataRef createCFDataFromCFString(CFStringRef string);

static inline void initpb(struct argblock *pbptr);
static const char *make_cstr_for_CFStr(CFStringRef in, CFStringEncoding encoding);
static const char *make_pasteboardID_cstr(struct argblock *pbptr);

int parsearg(const char *arg, struct argblock *pbptr);

int  copy(struct argblock *pbptr);
int paste(struct argblock *pbptr);
int paste_growl(struct argblock *pbptr);
int count(struct argblock *pbptr);
int  list(struct argblock *pbptr);
int clear(struct argblock *pbptr);
int  help(struct argblock *pbptr);
int version(struct argblock *pbptr);

const char *argv0 = NULL;

//This table swaps ^M (\x0d) and ^J (\x0a).
static const unsigned char nl_translate_table[256] = 
"\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\r\x0b\x0c\n\x0e\x0f"
"\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f"
"\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2a\x2b\x2c\x2d\x2e\x2f"
"\x30\x31\x32\x33\x34\x35\x36\x37\x38\x39\x3a\x3b\x3c\x3d\x3e\x3f"
"\x40\x41\x42\x43\x44\x45\x46\x47\x48\x49\x4a\x4b\x4c\x4d\x4e\x4f"
"\x50\x51\x52\x53\x54\x55\x56\x57\x58\x59\x5a\x5b\x5c\x5d\x5e\x5f"
"\x60\x61\x62\x63\x64\x65\x66\x67\x68\x69\x6a\x6b\x6c\x6d\x6e\x6f"
"\x70\x71\x72\x73\x74\x75\x76\x77\x78\x79\x7a\x7b\x7c\x7d\x7e\x7f"
"\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f"
"\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f"
"\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf"
"\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf"
"\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf"
"\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf"
"\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef"
"\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff";

static CFStringRef MacRoman_UTI = CFSTR("com.apple.traditional-mac-plain-text");

int main(int argc, const char **argv) {
	argv0 = argv[0];

	int retval = 0;

	initpb(&pb);
	
	while((--argc) && (pb.flags.phase != subcommand_options)) {
		if(retval = parsearg(*++argv, &pb))
			break;
	}
	//The subcommand gets our leftover arguments.
	pb.argc = argc;
	pb.argv = ++argv;

	OSStatus err;

	if(retval == 0) {
		if(pb.pasteboardID == NULL)
			pb.pasteboardID = CFRetain(kPasteboardClipboard);

		err = PasteboardCreate(pb.pasteboardID, &(pb.pasteboard));
		if(err != noErr) {
			fprintf(stderr, "%s: could not create pasteboard reference for pasteboard ID %s", argv0, make_pasteboardID_cstr(&pb));
			retval = 1;
		}
	}

	if(retval == 0) {
		if(pb.proc == NULL) {
//			fprintf(stderr, "%s: you must supply a subcommand (copy, paste, or clear)\n", argv0);
//			retval = 1;
			if(!isatty(pb.in_fd))
				retval = copy(&pb);
			//Paste when...
			//- the output is not a tty, OR
			//- the input was a tty (which means we didn't copy)
			if((retval == 0) && (!isatty(pb.out_fd)) || isatty(pb.in_fd))
				retval = paste(&pb);
		} else {
			retval = pb.proc(&pb);
		}
		if(pb.pasteboard)
			CFRelease(pb.pasteboard);
	}
	if(pb.pasteboardID)
		CFRelease(pb.pasteboardID);
	if(pb.type)
		CFRelease(pb.type);
	if(pb.in_fd > 2)
		close(pb.in_fd);
	if(pb.out_fd > 2)
		close(pb.out_fd);

	pb_deallocateall();
    return retval;
}

#pragma mark -

Boolean testarg(const char *a, const char *b, const char **param) {
	while((*a) && (*a == *b) && (*b != '=')) {
		++a, ++b;
	}

	if(param) *param = (*a == '=') ? &a[1] : NULL;
	return *a == *b;
}

static inline void initpb(struct argblock *pbptr) {
	pbptr->proc = NULL;

	pbptr->in_fd  =  STDIN_FILENO;
	pbptr->out_fd = STDOUT_FILENO;

	pbptr->pasteboard                     = NULL;

	pbptr->pasteboardID                   =
	pbptr->type                           = NULL;
	pbptr->pasteboardID_cstr              = NULL;

	pbptr->flags.reserved                 = 0U;
	pbptr->flags.phase                    = global_options;
	pbptr->flags.has_args                 = false;
	pbptr->flags.infer_translate_newlines = true;
	pbptr->flags.translate_newlines       = true;
}

int parsearg(const char *arg, struct argblock *pbptr) {
	const char *param;
	switch(pbptr->flags.phase) {
		case global_options:
			if(testarg(arg, "--type=", &param)) {
				pbptr->type = CFStringCreateWithCString(kCFAllocatorDefault, param, kCFStringEncodingUTF8);
				if(pbptr->flags.infer_translate_newlines)
					pbptr->flags.translate_newlines = UTTypeConformsTo(pbptr->type, CFSTR("public.text"));
			} else if(testarg(arg, "--pasteboard=", &param)) {
				pbptr->pasteboardID_cstr = param;
				pbptr->pasteboardID = CFStringCreateWithCString(kCFAllocatorDefault, param, kCFStringEncodingUTF8);
			} else if(testarg(arg, "--in-file=", &param))
				pbptr->in_fd = open(param, O_RDONLY, 0644);
			else if(testarg(arg, "--out-file=", &param))
				pbptr->out_fd = open(param, O_WRONLY | O_CREAT, 0644);
			else if(testarg(arg, "--no-translate-newlines", NULL)) {
				pbptr->flags.infer_translate_newlines = false;
				pbptr->flags.translate_newlines       = false;
			} else if(testarg(arg, "--translate-newlines", NULL)) {
				pbptr->flags.infer_translate_newlines = false;
				pbptr->flags.translate_newlines       = true;
			} else if(testarg(arg, "copy", NULL)
				 || testarg(arg, "paste", NULL)
				 || testarg(arg, "paste-growl", NULL)
				 || testarg(arg, "clear", NULL)
				 || testarg(arg, "count", NULL)
				 || testarg(arg, "list", NULL)
				 || testarg(arg, "help", NULL)
				 || testarg(arg, "--version", NULL))
			{
				++(pbptr->flags.phase);
				goto handle_subcommand;
			} else {
				fprintf(stderr, "%s: unrecognised global option '%s'\n", argv0, arg);
				return 1;
			}
			break;

		case subcommand:
		handle_subcommand:
			if(pbptr->proc == NULL) {
				if(testarg(arg, "copy", NULL))
					pbptr->proc = copy;
				else if(testarg(arg, "paste", NULL))
					pbptr->proc = paste;
				else if(testarg(arg, "paste-growl", NULL))
					pbptr->proc = paste_growl;
				else if(testarg(arg, "clear", NULL))
					pbptr->proc = clear;
				else if(testarg(arg, "count", NULL))
					pbptr->proc = count;
				else if(testarg(arg, "list", NULL))
					pbptr->proc = list;
				else if(testarg(arg, "help", NULL))
					pbptr->proc = help;
				else if(testarg(arg, "--version", NULL))
					pbptr->proc = version;
				else {
					fprintf(stderr, "%s: unrecognised subcommand '%s'\n", argv0, arg);
					return 1;
				}
				++(pbptr->flags.phase);
			} else {
				fprintf(stderr, "%s: duplicate subcommand '%s'\n", argv0, arg);
				return 1;
			}
			break;

		case subcommand_options:
			pbptr->flags.has_args = true;
			break;
	} //switch(pbptr->flags.phase)
	return 0;
}

#pragma mark -

static const char *make_cstr_for_CFStr(CFStringRef in, CFStringEncoding encoding) {
	const char *result = NULL;
	if(in) {
		result = CFStringGetCStringPtr(in, encoding);
		if(result == NULL) {
			CFRange IDrange = CFRangeMake(0, CFStringGetLength(in));
			CFIndex numBytes = 0;
			CFStringGetBytes(in, IDrange, encoding, /*lossByte*/ 0U, /*isExternalRepresentation*/ false, /*buffer*/ NULL, /*maxBufLen*/ 0, &numBytes);
			char *buf = pb_allocate(numBytes + 1U);
			if(buf) {
				CFIndex numChars = CFStringGetBytes(in, IDrange, encoding, /*lossByte*/ 0U, /*isExternalRepresentation*/ false, (unsigned char *)buf, /*maxBufLen*/ numBytes, &numBytes);
				buf[numBytes] = 0;
			}
			result = buf;
		}
	}
	return result;
}
static const char *make_pasteboardID_cstr(struct argblock *pbptr) {
	if(pbptr->pasteboardID_cstr == NULL)
		pbptr->pasteboardID_cstr = make_cstr_for_CFStr(pbptr->pasteboardID, kCFStringEncodingUTF8);
	return pbptr->pasteboardID_cstr;
}

static PasteboardItemID getRandomPasteboardItemID(void) {
	//Item IDs are determined by the application creating the item.
	//Their meaning is up to that same application.
	//pb doesn't need to associate any meaning with the item, so we just pull
	//  the ID out of a hat.

	PasteboardItemID item;

	srandom(time(NULL));
	item = (PasteboardItemID)random();

	//Random returns a long, which is 4 bytes.
	//But on Tiger, pointers can be 8 bytes, and PasteboardItemID is a pointer.
	if(sizeof(PasteboardItemID) > sizeof(int)) {
		ptrdiff_t itemInt = (((ptrdiff_t)item) << ((sizeof(PasteboardItemID) - sizeof(int)) * 8U)) ^ random();
		item = (PasteboardItemID)itemInt;
	}

	//An item ID of 0 is illegal. Make sure it doesn't happen.
	if(item == 0)
		++item;

	return item;
}

#pragma mark -

int copy(struct argblock *pbptr) {
	char *buf = NULL;
	size_t total_size = 0U;
	Boolean mapped = false;
	if(pbptr->in_fd == STDIN_FILENO) {
		enum { increment = 1048576U };
		ssize_t amt_read = 0;
		size_t bufsize = 0U;
		do {
			if(total_size % increment == 0U)
				buf = realloc(buf, bufsize += increment);
			total_size += amt_read = read(pbptr->in_fd, &buf[total_size], bufsize - (total_size % increment));
		} while(amt_read);
		printf("read %zu bytes from stdin\n", total_size);
	} else {
		//This is a regular file. Map it.
		struct stat sb;
		int retval = fstat(pbptr->in_fd, &sb);
		if(retval) {
			fprintf(stderr, "%s copy: could not perform fstat to determine file size: %s\n", argv0, strerror(errno));
			return 2;
		}
		total_size = sb.st_size;
		buf = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_FILE, pbptr->in_fd, /*offset*/ 0);
		if(buf == NULL) {
			fprintf(stderr, "%s copy: could not mmap %zu bytes of file: %s\n", argv0, total_size, strerror(errno));
			return 2;
		}
		mapped = true;
		printf("mapped %zu bytes from file\n", total_size);
	}

	OSStatus err;
	int retval = 0;

	err = PasteboardClear(pbptr->pasteboard);
	if(err != noErr) {
		fprintf(stderr, "%s copy: could not clear pasteboard %s because PasteboardClear returned %li\n", argv0, make_pasteboardID_cstr(pbptr), (long)err);
		return 2;
	}

	if(pbptr->flags.translate_newlines) {
		unsigned char *ptr = (unsigned char *)buf;

		for(unsigned long long i = 0ULL; i < total_size; ++i) {
			*ptr = nl_translate_table[*ptr];
			++ptr;
		}
	}

	PasteboardItemID item = getRandomPasteboardItemID();

	CFStringRef string;
	CFDataRef data = NULL;
	if(pbptr->type == NULL) {
		//Assume it's UTF-8. Convert it to UTF-16.
		string = CFStringCreateWithBytes(kCFAllocatorDefault, (const unsigned char *)buf, total_size, kCFStringEncodingUTF8, /*isExternalRepresentation*/ false);
		if(string == NULL) {
			//So much for that. Call it MacRoman and copy the pure bytes.
			pbptr->type = CFRetain(MacRoman_UTI);
			goto pure_data;
		}
		data = CFStringCreateExternalRepresentation(kCFAllocatorDefault, string, kCFStringEncodingUnicode, /*lossByte*/ 0U);
		pbptr->type = CFRetain(kUTTypeUTF16ExternalPlainText);
	}
	if(!data) {
pure_data:
		data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const unsigned char *)buf, total_size, /*bytesDeallocator*/ kCFAllocatorNull);
		if(data == NULL)
			data = CFDataCreate(kCFAllocatorDefault, (const unsigned char *)buf, total_size);
		if(data == NULL) {
			fprintf(stderr, "%s copy: could not create CFData object for copy to pasteboard %s\n", argv0, make_pasteboardID_cstr(pbptr));
			return 2;
		}
	}

	//Always do this first.
	err = PasteboardPutItemFlavor(pbptr->pasteboard, item, pbptr->type, data, kPasteboardFlavorNoFlags);
	if(err != noErr) {
		fprintf(stderr, "%s copy: could not copy data for main type \"%s\": PasteboardPutItemFlavor returned error %li\n", argv0, make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8), (long)err);
	}

	//Translate encodings.
	CFDataRef UTF16Data = NULL, UTF16ExtData = NULL, UTF8Data = NULL, MacRomanData = NULL;
	Boolean typeIsUTF16 = false, typeIsUTF16Ext = false, typeIsUTF8 = false, typeIsMacRoman = false;
	Boolean isTextData = false; //Note: We can't just test conformance to public.text because that includes formats like public.rtf.
	fprintf(stderr, "pbptr->type is %s\n", make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8));
	if(UTTypeConformsTo(pbptr->type, kUTTypeUTF16PlainText)) {
		UTF16Data = data;
		isTextData = typeIsUTF16 = true;
	} else if(UTTypeConformsTo(pbptr->type, kUTTypeUTF16ExternalPlainText)) {
		UTF16ExtData = data;
		isTextData = typeIsUTF16Ext = true;
	} else if(UTTypeConformsTo(pbptr->type, kUTTypeUTF8PlainText)) {
		UTF8Data = data;
		isTextData = typeIsUTF8 = true;
	} else if(UTTypeConformsTo(pbptr->type, MacRoman_UTI)) {
		MacRomanData = data;
		isTextData = typeIsMacRoman = true;
	}
	fprintf(stderr, "typeIsUTF16: %hhu; typeIsUTF16Ext: %hhu; typeIsUTF8: %hhu; typeIsMacRoman: %hhu \n", typeIsUTF16, typeIsUTF16Ext, typeIsUTF8, typeIsMacRoman);
	if(isTextData) {
		convert_encodings(&UTF16Data, &UTF16ExtData, &UTF8Data, &MacRomanData);
		fprintf(stderr, "data lengths: UTF-16 %lli, UTF-16 ext %lli, UTF-8 %lli, MacRoman %lli \n", (long long)(UTF16Data ? CFDataGetLength(UTF16Data) : -1LL), (long long)(UTF16ExtData ? CFDataGetLength(UTF16ExtData) : -1LL), (long long)(UTF8Data ? CFDataGetLength(UTF8Data) : -1LL), (long long)(MacRomanData ? CFDataGetLength(MacRomanData) : -1LL));
		//Only copy it if we did not already copy it (which we have done if its type is pbptr->type).
		if(UTF16Data && !typeIsUTF16) {
			err = PasteboardPutItemFlavor(pbptr->pasteboard, item, kUTTypeUTF16PlainText, UTF16Data, kPasteboardFlavorSenderTranslated);
			if(err != noErr) {
				fprintf(stderr, "%s copy: could not copy alternate \"%s\" data for main type \"%s\": PasteboardPutItemFlavor returned error %li\n", argv0, make_cstr_for_CFStr(kUTTypeUTF16PlainText, kCFStringEncodingUTF8), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8), (long)err);
				err = noErr; //These aren't critically-important.
			}
		}
		if(UTF16ExtData && !typeIsUTF16Ext) {
			err = PasteboardPutItemFlavor(pbptr->pasteboard, item, kUTTypeUTF16ExternalPlainText, UTF16ExtData, kPasteboardFlavorSenderTranslated);
			if(err != noErr) {
				fprintf(stderr, "%s copy: could not copy alternate \"%s\" data for main type \"%s\": PasteboardPutItemFlavor returned error %li\n", argv0, make_cstr_for_CFStr(kUTTypeUTF16ExternalPlainText, kCFStringEncodingUTF8), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8), (long)err);
				err = noErr; //These aren't critically-important.
			}
		}
		if(UTF8Data && !typeIsUTF8) {
			err = PasteboardPutItemFlavor(pbptr->pasteboard, item, kUTTypeUTF8PlainText, UTF8Data, kPasteboardFlavorSenderTranslated);
			if(err != noErr) {
				fprintf(stderr, "%s copy: could not copy alternate \"%s\" data for main type \"%s\": PasteboardPutItemFlavor returned error %li\n", argv0, make_cstr_for_CFStr(kUTTypeUTF8PlainText, kCFStringEncodingUTF8), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8), (long)err);
				err = noErr; //These aren't critically-important.
			}
		}
		if(MacRomanData && !typeIsMacRoman) {
			err = PasteboardPutItemFlavor(pbptr->pasteboard, item, MacRoman_UTI, MacRomanData, kPasteboardFlavorSenderTranslated);
			if(err != noErr) {
				fprintf(stderr, "%s copy: could not copy alternate \"%s\" data for main type \"%s\": PasteboardPutItemFlavor returned error %li\n", argv0, make_cstr_for_CFStr(MacRoman_UTI, kCFStringEncodingUTF8), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8), (long)err);
				err = noErr; //These aren't critically-important.
			}
		}
	}

	CFRelease(data);

	if(mapped)
		munmap(buf, total_size);
	else
		free(buf);

	if(err != noErr) {
		fprintf(stderr, "%s copy: could not copy to pasteboard %s because PasteboardPutItemFlavor returned %li\n", argv0, make_pasteboardID_cstr(pbptr), err);
		retval = 2;
	}

	return retval;
}
int paste_one(struct argblock *pbptr, UInt32 index) {
	int retval = 0;
	OSStatus err;

	PasteboardItemID item;
	err = PasteboardGetItemIdentifier(pbptr->pasteboard, index, &item);
	if(err != noErr) {
		fprintf(stderr, "%s: can't find item %lu on pasteboard %s: PasteboardGetItemIdentifier returned %li\n", argv0, (unsigned long)index, make_pasteboardID_cstr(pbptr), (long)err);
		return 2;
	}

	CFDataRef data = NULL;
	fprintf(stderr, "%s: Pasting item %u of pasteboard \"%s\" in flavor type \"%s\".\n", argv0, index, make_pasteboardID_cstr(pbptr), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8));
	if(pbptr->type == NULL) {
		CFDataRef UTF8Data = NULL;
		err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypeUTF8PlainText, &UTF8Data);
		if(UTF8Data)
			pbptr->type = CFRetain(kUTTypeUTF8PlainText);
		else {
			//Look for UTF-16, then UTF-16 with BOM, then MacRoman. Convert the first of those that we find (if any) to UTF-8.
			CFDataRef UTF16Data = NULL, UTF16ExtData = NULL, MacRomanData = NULL;
			err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypeUTF16PlainText, &UTF16Data);
			if(!UTF16Data)
				err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypeUTF16ExternalPlainText, &UTF16ExtData);
			if(!UTF16ExtData)
				err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, MacRoman_UTI, &MacRomanData);
			//If we have anything, convert it to UTF-8.
			if(UTF16Data || UTF16ExtData || MacRomanData) {
				convert_encodings(UTF16Data ? &UTF16Data : NULL,
			                  	  UTF16ExtData ? &UTF16ExtData : NULL,
			                  	  &UTF8Data,
			                  	  MacRomanData ? &MacRomanData : NULL);
				pbptr->type = CFRetain(kUTTypeUTF8PlainText);
				//Clean up after PasteboardCopyItemFlavorData.
				if(UTF16Data)    CFRelease(UTF16Data);
				if(UTF16ExtData) CFRelease(UTF16ExtData);
				if(MacRomanData) CFRelease(MacRomanData);
			}
		}
		data = UTF8Data;
	} else {
		//There is an explicit type.
		err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, pbptr->type, &data);
	}

	if(err != noErr) {
		if(err == badPasteboardFlavorErr)
			fprintf(stderr, "%s: could not paste item %u of pasteboard \"%s\": it does not exist in flavor type \"%s\".\n", argv0, index, make_pasteboardID_cstr(pbptr), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8), (long)err);
		else
			fprintf(stderr, "%s: could not paste item %u of pasteboard \"%s\": PasteboardCopyItemFlavorData (for flavor type \"%s\") returned error %li\n", argv0, index, make_pasteboardID_cstr(pbptr), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8), (long)err);
		retval = 2;
	} else {
		CFIndex length = CFDataGetLength(data);
		const unsigned char *rptr = CFDataGetBytePtr(data);
		void *buf = NULL;
		if(pbptr->flags.translate_newlines) {
			buf = malloc(length);
			unsigned char *wptr = buf;

			for(unsigned long long i = 0ULL; i < length; ++i)
				*(wptr++) = nl_translate_table[*(rptr++)];

			rptr = buf;
		}

		write(pbptr->out_fd, rptr, length);

		if(buf)
			free(buf);
	}

	if(data)
		fprintf(stderr, "data's retain count: %lli\n", (long long)CFGetRetainCount(data));
//		CFRelease(data);

	return retval;
}
int paste(struct argblock *pbptr) {
	int retval = 0;

	UInt32 index = 0U; //Index of the item to paste.

	if(pbptr->argc)
		if(index = strtoul(*(pbptr->argv), NULL, 0)) {
			++(pbptr->argv); --(pbptr->argc);
		}
	if(pbptr->argc)
		if(strchr(*(pbptr->argv), '.')) {
			if(pbptr->type != NULL) {
				//This is a filename.
				pbptr->out_fd = open(*(pbptr->argv), O_WRONLY | O_CREAT, 0644);
			} else {
				//UTI
				pbptr->type = CFStringCreateWithCString(kCFAllocatorDefault, *pbptr->argv, kCFStringEncodingUTF8);
			}
			++(pbptr->argv); --(pbptr->argc);
		}
	if(pbptr->argc)
		if(pbptr->out_fd < 0 || pbptr->out_fd == STDOUT_FILENO)
			pbptr->out_fd = open(*(pbptr->argv), O_WRONLY | O_CREAT, 0644);

	//Make sure we paste into an empty file.
	//Why not truncate stdout? If we do, >> doesn't work.
	if(pbptr->out_fd != STDOUT_FILENO)
		ftruncate(pbptr->out_fd, 0);

	OSStatus err;
	ItemCount numItems;
//	printf("pasteboard: %p\n", pbptr->pasteboard);
	err = PasteboardGetItemCount(pbptr->pasteboard, &numItems);
	if(index) {
		if(err == noErr && index > numItems) {
			fprintf(stderr, "%s: there are only %u items on pasteboard \"%s\"\n", argv0, numItems, make_pasteboardID_cstr(pbptr));
			return 1;
		} else {
			//Note that if we can't determine how many items there are, we forge ahead anyway.
			return paste_one(pbptr, index);
		}
	} else {
		if(err != noErr) {
			fprintf(stderr, "%s: could not determine how many items are on pasteboard %s: PasteboardGetItemCount returned %li\n", argv0, make_pasteboardID_cstr(pbptr), (long)err);
			return 2;
		}

		while(index < numItems) {
			retval = paste_one(pbptr, ++index);
			if(retval) {
				fprintf(stderr, "%s: error pasting item %u from pasteboard \"%s\"\n", argv0, index, make_pasteboardID_cstr(pbptr));
				break;
			}
		}
	}

	return retval;
}
int paste_growl(struct argblock *pbptr) {
	fputs("paste_growl called\n", stdout);
	int retval = 0;
	
	UInt32 index = 0U; //Index of the item to paste.
	
	if(pbptr->argc)
		if(index = strtoul(*(pbptr->argv), NULL, 0)) {
			++(pbptr->argv); --(pbptr->argc);
		}
	if(pbptr->argc)
		if(strchr(*(pbptr->argv), '.')) {
			if(pbptr->type != NULL) {
				//This is a filename.
				pbptr->out_fd = open(*(pbptr->argv), O_WRONLY | O_CREAT, 0644);
			} else {
				//UTI.
				pbptr->type = CFStringCreateWithCString(kCFAllocatorDefault, *pbptr->argv, kCFStringEncodingUTF8);
			}
			++(pbptr->argv); --(pbptr->argc);
		}

	OSStatus err;
	ItemCount numItems;
//	printf("pasteboard: %p\n", pbptr->pasteboard);
	err = PasteboardGetItemCount(pbptr->pasteboard, &numItems);
	if(index) {
		if(err == noErr && index > numItems) {
			fprintf(stderr, "%s: there are only %u items on pasteboard \"%s\"\n", argv0, numItems, make_pasteboardID_cstr(pbptr));
			return 1;
		} else {
			//Note that if we can't determine how many items there are, we forge ahead anyway.
			return paste_one(pbptr, index);
		}
	} else {
		if(err != noErr) {
			fprintf(stderr, "%s: could not determine how many items are on pasteboard %s: PasteboardGetItemCount returned %li\n", argv0, make_pasteboardID_cstr(pbptr), (long)err);
			return 2;
		}
		
		CFStringRef title = NULL;
		CFStringRef desc = NULL;
		CFDataRef imageData = NULL;

		while((++index <= numItems) && !(imageData && desc)) {
			PasteboardItemID item;
			err = PasteboardGetItemIdentifier(pbptr->pasteboard, index, &item);
			if(err != noErr) {
				fprintf(stderr, "%s: can't find item %lu on pasteboard %s: PasteboardGetItemIdentifier returned %li\n", argv0, (unsigned long)index, make_pasteboardID_cstr(pbptr), (long)err);
				return 2;
			}

			CFDataRef stringData = NULL;
			CFStringEncoding stringEncoding = 0;

			if(!desc) {
				//First, UTF-16.
				err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypeUTF16PlainText, &stringData);
				if(err == noErr)
					stringEncoding = kCFStringEncodingUnicode;
				else {
					//Failing that, UTF-8.
					err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypeUTF8PlainText, &stringData);
					if(err == noErr)
						stringEncoding = kCFStringEncodingUTF8;
					else {
						//Finally, MacRoman.
						err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, MacRoman_UTI, &stringData);
						if(err == noErr)
							stringEncoding = kCFStringEncodingMacRoman;
					}
				} 
			}
			if(!imageData) {
				//First, vector formats, since they work well at any size.

				//First, PDF.
				err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypePDF, &imageData);
				if(err != noErr) {
					//Failing that, PICT.
					err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypePICT, &imageData);
				}

				//OK, that won't work. Try raster formats now.

				//First, IconFamily.
				if(err != noErr) {
					err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypeAppleICNS, &imageData);
				}
				if(err != noErr) {
					//Failing that, TIFF.
					err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypeTIFF, &imageData);
				}
				if(err != noErr) {
					//Failing that, QuickTime image.
					err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypeQuickTimeImage, &imageData);
				}
				if(err != noErr) {
					//Failing that, PNG.
					err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, kUTTypePNG, &imageData);
				}
			}

			if(err != noErr) {
				fprintf(stderr, "%s: could not paste item %u of pasteboard \"%s\": PasteboardCopyItemFlavorData (for flavor type \"%s\") returned error %li\n", argv0, index, make_pasteboardID_cstr(pbptr), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8), (long)err);
				retval = 2;
			} else if(stringData) {
				if(pbptr->flags.translate_newlines) {
					CFIndex length = CFDataGetLength(stringData);
					CFMutableDataRef mutableStringData = CFDataCreateMutableCopy(kCFAllocatorDefault, length, stringData);
					unsigned char *ptr = CFDataGetMutableBytePtr(mutableStringData);

					for(CFIndex i = 0LL; i < length; ++i)
						*(ptr++) = nl_translate_table[*(ptr++)];

					CFRelease(stringData);
					stringData = mutableStringData;
				}

				if(stringData) {
					//Create the notification description.
					desc = CFStringCreateFromExternalRepresentation(kCFAllocatorDefault, stringData, stringEncoding);
					printf("created description %p from data %p in encoding %u\n", desc, stringData, stringEncoding);
					CFRelease(stringData);
				}
			}

			if(retval) {
				fprintf(stderr, "%s: error pasting item %u from pasteboard \"%s\"\n", argv0, index, make_pasteboardID_cstr(pbptr));
				break;
			}
		} //while((index < numItems) && !(imageData && desc))

		CFStringRef notificationName = CFSTR("pb Growl output");
		CFArrayRef notificationNames = CFArrayCreate(kCFAllocatorDefault, (const void **)&notificationName, 1, &kCFTypeArrayCallBacks);
		CFMutableDictionaryRef userInfo = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFNotificationCenterRef dnc = CFNotificationCenterGetDistributedCenter();

		//Register with Growl.
		CFDictionarySetValue(userInfo, GROWL_APP_NAME, CFSTR("pb"));
		CFDictionarySetValue(userInfo, GROWL_NOTIFICATIONS_ALL, notificationNames);
		CFDictionarySetValue(userInfo, GROWL_NOTIFICATIONS_DEFAULT, notificationNames);
		CFNotificationCenterPostNotification(dnc, GROWL_APP_REGISTRATION, /*object*/ NULL, userInfo, /*deliverImmediately*/ false);

		CFDictionaryRemoveValue(userInfo, GROWL_NOTIFICATIONS_ALL);
		CFDictionaryRemoveValue(userInfo, GROWL_NOTIFICATIONS_DEFAULT);

		//Post the notification.
		CFDictionarySetValue(userInfo, GROWL_NOTIFICATION_NAME, notificationName);
		CFDictionarySetValue(userInfo, GROWL_NOTIFICATION_TITLE, pbptr->pasteboardID);
		printf("desc: %p\nimageData: %p\n", desc, imageData);
		if(desc) {
			CFDictionarySetValue(userInfo, GROWL_NOTIFICATION_DESCRIPTION, desc);
			CFRelease(desc);
		}
		if(imageData) {
			CFDictionarySetValue(userInfo, GROWL_NOTIFICATION_ICON, imageData);
			CFRelease(imageData);
		}
		CFNotificationCenterPostNotification(dnc, GROWL_NOTIFICATION, /*object*/ NULL, userInfo, /*deliverImmediately*/ false);

		CFRelease(notificationNames);
		CFRelease(userInfo);
	} //if(!index)

	return retval;
}
int count(struct argblock *pbptr) {
	ItemCount num;
	OSStatus err = PasteboardGetItemCount(pbptr->pasteboard, &num);
	if(err != noErr) {
		fprintf(stderr, "%s count: PasteboardGetItemCount for pasteboard %s returned %li\n", argv0, make_pasteboardID_cstr(pbptr), (long)err);
		return 2;
	}
	printf("%lu\n", (unsigned long)num);
	return 0;
}
int list(struct argblock *pbptr) {
	ItemCount num;
	OSStatus err = PasteboardGetItemCount(pbptr->pasteboard, &num);
	if(err != noErr) {
		fprintf(stderr, "%s list: PasteboardGetItemCount for pasteboard %s returned %li\n", argv0, make_pasteboardID_cstr(pbptr), (long)err);
		return 2;
	}
	CFShow(pbptr->pasteboardID);
	printf("%lu items\n", (unsigned long)num);
	if(num) {
		for(UInt32 i = 1U; i <= num; ++i) {
			CFArrayRef flavors = NULL;
			PasteboardItemID item = NULL;

			err = PasteboardGetItemIdentifier(pbptr->pasteboard, i, &item);
			if(err != noErr) {
				fprintf(stderr, "%s list: PasteboardGetItemIdentifier for pasteboard %s item %lu returned %li\n", argv0, make_pasteboardID_cstr(pbptr), (unsigned long)i, (long)err);
				break;
			}

			err = PasteboardCopyItemFlavors(pbptr->pasteboard, item, &flavors);
			if(err != noErr) {
				fprintf(stderr, "%s list: PasteboardCopyItemFlavors for pasteboard %s item %lu (object address %p) returned %li\n", argv0, make_pasteboardID_cstr(pbptr), (unsigned long)i, item, (long)err);
				break;
			}

			CFIndex numFlavors = CFArrayGetCount(flavors);
			printf("\n#%u: %lu flavors\n", i, numFlavors);
			for(CFIndex j = 0U; j < numFlavors; ++j) {
				CFStringRef flavor = CFArrayGetValueAtIndex(flavors, j);
				const char *flavor_c = make_cstr_for_CFStr(flavor, kCFStringEncodingUTF8);

				//Make this next step optional! (list --verbose, maybe)
				//If verbose...
				CFDataRef flavorData = NULL;
				err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, flavor, &flavorData);

				if(err == noErr) {
					printf("\t%s (%lli bytes)\n", flavor_c, (long long)CFDataGetLength(flavorData));
					CFRelease(flavorData);
				} else
					printf("\t%s (??? bytes; PasteboardCopyItemFlavorData returned %li)\n", flavor_c, (long)err);
				//else...
				//	printf("\t%s\n", flavor_c);
			}
		}
	}

	return 0;
}
int clear(struct argblock *pbptr) {
	OSStatus err = PasteboardClear(pbptr->pasteboard);
	if(err != noErr) {
		fprintf(stderr, "%s clear: PasteboardClear for pasteboard %s returned %li\n", argv0, make_pasteboardID_cstr(pbptr), (long)err);
		return 2;
	} else
		return 0;
}
int help(struct argblock *pbptr) {
	printf("usage: %s [global-options] subcommand [options]\n"
		   "global-options:\n"
		   "\t--type=UTI\tspecify the type of the data you're handling\n"
		   "\t--pasteboard=ID\tspecify the pasteboard on which to operate\n"
		   "\t\tstandard pasteboards:\n"
		   "\t\tcom.apple.pasteboard.clipboard (default)\n"
		   "\t\tcom.apple.pasteboard.find\n"
		   "\t--file=path\tspecify the path to a file to use for I/O instead of stdio\n"
		   "subcommands:\n"
		   "\tcopy [UTI] [path]\n"
		   "\t\tread from the specified file/stdin and copy as the specified flavor type/UTF-8\n"
		   "\tpaste [index] [UTI] [path]\n"
		   "\t\twrite the contents of the specified item/all items in the specified flavor type/any text type to the specified file/stdout\n"
		   "\tclear\n"
		   "\t\tremove all items from the pasteboard\n"
		   "\tcount\n"
		   "\t\tshow the number of items on the pasteboard\n"
		   "\tlist [index]\n"
		   "\t\tshow all available flavor types of all items/the specified item (1-based)\n"
		   "\thelp\n"
		   "\t\tview this help\n",
		   argv0);
	return 0;
}
int version(struct argblock *pbptr) {
	printf("pb version 0.1\n"
		   "copyright 2004 Mac-arena the Bored Zo\n"
		   "interface to the Pasteboard Manager:\n"
		   "read and write pasteboards (including the clipboard)\n"
		   "\n"
		   "type %s help for usage\n", argv0);
	fputs("EXPERIMENTAL\n", stdout);
	return 0;
}

#pragma mark -

struct allocation {
	void *ptr;
	struct allocation *next;
} *firstAllocation, *lastAllocation;

static void *pb_allocate(size_t nbytes) {
	void *ptr = malloc(nbytes);
	if(ptr) {
		struct allocation *newAllocation = malloc(sizeof(struct allocation));
		newAllocation->next = NULL;
		newAllocation->ptr  = ptr;
		if(lastAllocation)
			lastAllocation->next = newAllocation;
		lastAllocation = newAllocation;
		if(!firstAllocation)
			firstAllocation = newAllocation;
	}
	return ptr;
}
static void pb_deallocate(void *buf) {
	struct allocation *allocation = firstAllocation;
	if(!allocation) {
		//No allocations in the list; just free it.
		free(buf);
	} else if(allocation->ptr == buf) {
		if(allocation == lastAllocation)
			lastAllocation = NULL;
		firstAllocation = allocation->next;
		free(buf);
		free(allocation);
	} else {
		while(allocation) {
			if(allocation->next) {
				struct allocation *nextAllocation = allocation->next;

				if(nextAllocation->ptr == buf) {
					allocation->next = nextAllocation->next;
					if(nextAllocation == lastAllocation)
						lastAllocation = NULL;
					free(nextAllocation);
					free(buf);
				}
			}
		}
	}
}
static void pb_deallocateall(void) {
	struct allocation *allocation = firstAllocation;
	while(allocation) {
		struct allocation *nextAllocation = allocation->next;
		free(allocation);
		allocation = nextAllocation;
	}
}

static Boolean convert_encodings(CFDataRef *inoutUTF16Data, CFDataRef *inoutUTF16ExtData, CFDataRef *inoutUTF8Data, CFDataRef *inoutMacRomanData) {
	//If a format is not requested, it has not failed, and so we should consider it to have succeeded, so we set its variable to true.
	//But if it is requested, it has not succeeded until it has been attempted, so we set its variable to false.
	Boolean success_UTF16    = ((!inoutUTF16Data)    || *inoutUTF16Data);
	Boolean success_UTF16Ext = ((!inoutUTF16ExtData) || *inoutUTF16ExtData);
	Boolean success_UTF8     = ((!inoutUTF8Data)     || *inoutUTF8Data);
	Boolean success_MacRoman = ((!inoutMacRomanData) || *inoutMacRomanData);

	CFStringRef string = NULL;

	//Convert UTF-16 (with BOM), UTF-8, or MacRoman to UTF-16.
	if(inoutUTF16Data && !*inoutUTF16Data) {
		if(!string) {
			if(inoutUTF16ExtData && *inoutUTF16ExtData) {
				string = CFStringCreateFromExternalRepresentation(kCFAllocatorDefault,
			                                 	 	 	 		  *inoutUTF16ExtData,
			                                 	 	 	 		  kCFStringEncodingUnicode);
			} else if(inoutUTF8Data && *inoutUTF8Data) {
				string = CFStringCreateWithBytes(kCFAllocatorDefault,
			                                 	 CFDataGetBytePtr(*inoutUTF8Data),
			                                 	 CFDataGetLength(*inoutUTF8Data),
			                                 	 kCFStringEncodingUTF8,
			                                 	 false);
			} else if(inoutMacRomanData && *inoutMacRomanData) {
				string = CFStringCreateWithBytes(kCFAllocatorDefault,
			                                 	 CFDataGetBytePtr(*inoutMacRomanData),
			                                 	 CFDataGetLength(*inoutMacRomanData),
			                                 	 kCFStringEncodingMacRoman,
			                                 	 false);
			}
		}

		if(string) {
			*inoutUTF16Data = createCFDataFromCFString(string);
			success_UTF16 = (*inoutUTF16Data != NULL);
		}
	}

	//Convert UTF-16, UTF-8, or MacRoman to UTF-16 (with BOM).
	if(inoutUTF16ExtData && !*inoutUTF16ExtData) {
		if(!string) {
			if(inoutUTF16Data && *inoutUTF16Data) {
				string = CFStringCreateWithCharacters(kCFAllocatorDefault,
			                                      	  (const UniChar *)CFDataGetBytePtr(*inoutUTF16Data),
			                                      	  CFDataGetLength(*inoutUTF16Data) / sizeof(UniChar));
			} else if(inoutUTF8Data && *inoutUTF8Data) {
				string = CFStringCreateWithBytes(kCFAllocatorDefault,
			                                 	 CFDataGetBytePtr(*inoutUTF8Data),
			                                 	 CFDataGetLength(*inoutUTF8Data),
			                                 	 kCFStringEncodingUTF8,
			                                 	 false);
			} else if(inoutMacRomanData && *inoutMacRomanData) {
				string = CFStringCreateWithBytes(kCFAllocatorDefault,
			                                 	 CFDataGetBytePtr(*inoutMacRomanData),
			                                 	 CFDataGetLength(*inoutMacRomanData),
			                                 	 kCFStringEncodingMacRoman,
			                                 	 false);
			}
		}

		if(string) {
			*inoutUTF16ExtData = CFStringCreateExternalRepresentation(kCFAllocatorDefault, string, kCFStringEncodingUnicode, /*lossByte*/ 0U);
			success_UTF16Ext = (*inoutUTF16ExtData != NULL);
		}
	}

	//Convert UTF-16, UTF-16 (with BOM), or MacRoman to UTF-8.
	if(inoutUTF8Data && !*inoutUTF8Data) {
		if(!string) {
			if(inoutUTF16Data && *inoutUTF16Data) {
				string = CFStringCreateWithCharacters(kCFAllocatorDefault,
			                                      	  (const UniChar *)CFDataGetBytePtr(*inoutUTF16Data),
			                                      	  CFDataGetLength(*inoutUTF16Data) / sizeof(UniChar));
			} else if(inoutUTF16ExtData && *inoutUTF16ExtData) {
				string = CFStringCreateFromExternalRepresentation(kCFAllocatorDefault,
			                                 	 	 	 		  *inoutUTF16ExtData,
			                                 	 	 	 		  kCFStringEncodingUnicode);
			} else if(inoutMacRomanData && *inoutMacRomanData) {
				string = CFStringCreateWithBytes(kCFAllocatorDefault,
			                                 	 CFDataGetBytePtr(*inoutMacRomanData),
			                                 	 CFDataGetLength(*inoutMacRomanData),
			                                 	 kCFStringEncodingMacRoman,
			                                 	 false);
			}
		}

		if(string) {
			CFRange range = { 0, CFStringGetLength(string) };
			CFIndex numBytes = 0;
			CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(range.length, kCFStringEncodingUTF8);

			CFIndex numCharsConverted = CFStringGetBytes(string,
			                                             range,
			                                             kCFStringEncodingUTF8,
			                                             /*lossByte*/ 0U,
			                                             /*isExternalRepresentation*/ false,
			                                             /*buffer*/ NULL,
			                                             /*maxBufLen*/ maxBytes,
			                                             &numBytes);
			if(numCharsConverted) {
				CFMutableDataRef mutableData = CFDataCreateMutable(kCFAllocatorDefault, numBytes);
				if(mutableData) {
					CFDataSetLength(mutableData, numBytes);
					numCharsConverted = CFStringGetBytes(string,
					                                     range,
					                                     kCFStringEncodingUTF8,
					                                     /*lossByte*/ 0U,
					                                     /*isExternalRepresentation*/ false,
					                                     CFDataGetMutableBytePtr(mutableData),
					                                     /*maxBufLen*/ numBytes,
					                                     &numBytes);
					*inoutUTF8Data = mutableData;/*CFDataCreateCopy(kCFAllocatorDefault, mutableData);
					CFRelease(mutableData);
				 	 */
					success_UTF8 = (*inoutUTF8Data != NULL);
				}
			}
		}
	}

	//Convert UTF-16, UTF-16 (with BOM), or UTF-8 to MacRoman.
	if(inoutMacRomanData && !*inoutMacRomanData) {
		if(!string) {
			if(inoutUTF16Data && *inoutUTF16Data) {
				string = CFStringCreateWithCharacters(kCFAllocatorDefault,
			                                      	  (const UniChar *)CFDataGetBytePtr(*inoutUTF16Data),
			                                      	  CFDataGetLength(*inoutUTF16Data) / sizeof(UniChar));
			} else if(inoutUTF16ExtData && *inoutUTF16ExtData) {
				string = CFStringCreateFromExternalRepresentation(kCFAllocatorDefault,
			                                 	 	 	 		  *inoutUTF16ExtData,
			                                 	 	 	 		  kCFStringEncodingUnicode);
			} else if(inoutUTF8Data && *inoutUTF8Data) {
				string = CFStringCreateWithBytes(kCFAllocatorDefault,
			                                 	 CFDataGetBytePtr(*inoutUTF8Data),
			                                 	 CFDataGetLength(*inoutUTF8Data),
			                                 	 kCFStringEncodingUTF8,
			                                 	 false);
			}
		}

		if(string) {
			CFRange range = { 0, CFStringGetLength(string) };
			CFIndex numBytes = 0;
			CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(range.length, kCFStringEncodingMacRoman);

			CFIndex numCharsConverted = CFStringGetBytes(string,
			                                             range,
			                                             kCFStringEncodingMacRoman,
			                                             /*lossByte*/ '?',
			                                             /*isExternalRepresentation*/ false,
			                                             /*buffer*/ NULL,
			                                             /*maxBufLen*/ maxBytes,
			                                             &numBytes);
			if(numCharsConverted) {
				CFMutableDataRef mutableData = CFDataCreateMutable(kCFAllocatorDefault, numBytes);
				if(mutableData) {
					CFDataSetLength(mutableData, numBytes);
					numCharsConverted = CFStringGetBytes(string,
					                                     range,
					                                     kCFStringEncodingMacRoman,
					                                     /*lossByte*/ '?',
					                                     /*isExternalRepresentation*/ false,
					                                     CFDataGetMutableBytePtr(mutableData),
					                                     /*maxBufLen*/ numBytes,
					                                     &numBytes);
					*inoutMacRomanData = mutableData;/*CFDataCreateCopy(kCFAllocatorDefault, mutableData);
					CFRelease(mutableData);
				 	 */
					success_MacRoman = (*inoutMacRomanData != NULL);
				}
			}
		}
	}

	if(string)
		CFRelease(string);

	return success_UTF16 && success_UTF16Ext && success_UTF8 && success_MacRoman;
}
static CFDataRef createCFDataFromCFString(CFStringRef string) {
	CFDataRef data = NULL;

	if(string) {
		CFRange range = { 0, CFStringGetLength(string) };
		CFIndex dataSize = range.length * sizeof(UniChar);
		CFMutableDataRef mutableData = CFDataCreateMutable(kCFAllocatorDefault, dataSize);
		if(mutableData) {
			CFDataSetLength(mutableData, dataSize);
			CFStringGetCharacters(string, range, (UniChar *)CFDataGetMutableBytePtr(mutableData));
			data = mutableData;/*CFDataCreateCopy(kCFAllocatorDefault, mutableData);
			CFRelease(mutableData);
			 */
		}
	}

	return data;
}
