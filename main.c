#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>
#include <sys/errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "compare_argument.h"

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

	CFIndex itemIndex;

	CFStringRef type; //UTI

	struct {
		unsigned reserved: 29;
		enum {
			global_options,
			subcommand,
			subcommand_options
		} phase: 2;
		unsigned has_args: 1;
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

//If the given C-string is not a known UTI, returns NULL. Otherwise returns a CFString for it.
static CFStringRef create_UTI_with_cstr(const char *arg);

static Boolean copy_type_by_filename(struct argblock *pbptr);

static inline void initpb(struct argblock *pbptr);
static const char *make_cstr_for_CFStr(CFStringRef in, CFStringEncoding encoding, void (**outDeallocator)(const char *ptr));
static const char *make_pasteboardID_cstr(struct argblock *pbptr, void (**outDeallocator)(const char *ptr));

int parsearg(const char *arg, struct argblock *pbptr);

int  copy(struct argblock *pbptr);
int paste(struct argblock *pbptr);
int count(struct argblock *pbptr);
int  list(struct argblock *pbptr);
int clear(struct argblock *pbptr);
int  help(struct argblock *pbptr);
int version(struct argblock *pbptr);

const char *argv0 = NULL;

static CFStringRef MacRoman_UTI = CFSTR("com.apple.traditional-mac-plain-text");

int main(int argc, const char **argv) {
	argv0 = argv[0];

	int retval = 0;

	initpb(&pb);
	
	while((--argc) && (pb.flags.phase != subcommand_options)) {
		if((retval = parsearg(*++argv, &pb)))
			break;
	}
	//The subcommand gets our leftover arguments.
	pb.argc = argc;
	pb.argv = ++argv;

	if (pb.in_fd == -1)
		pb.in_fd = STDIN_FILENO;
	if (pb.out_fd == -1)
		pb.out_fd = STDOUT_FILENO;

	OSStatus err;

	if(retval == 0) {
		if(pb.pasteboardID == NULL)
			pb.pasteboardID = CFRetain(kPasteboardClipboard);

		err = PasteboardCreate(pb.pasteboardID, &(pb.pasteboard));
		if(err != noErr) {
			fprintf(stderr, "%s: could not create pasteboard reference for pasteboard ID %s", argv0, make_pasteboardID_cstr(&pb, /*deallocator*/ NULL));
			retval = 1;
		}
	}

	if(retval == 0) {
		if(pb.proc == NULL) {
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
	if(pb.in_fd > -1)
		close(pb.in_fd);
	if(pb.out_fd > -1)
		close(pb.out_fd);

	pb_deallocateall();
    return retval;
}

#pragma mark -

Boolean testarg(const char *a, const char *b, const char **param) {
	while((*a) && (*a == *b) && (*b != '=')) {
		++a;
		++b;
	}

	if(param) *param = (*a == '=') ? &a[1] : NULL;
	return *a == *b;
}

static inline void initpb(struct argblock *pbptr) {
	pbptr->proc = NULL;

	pbptr->in_fd  = -1;
	pbptr->out_fd = -1;

	pbptr->pasteboard                     = NULL;

	pbptr->pasteboardID                   =
	pbptr->type                           = NULL;
	pbptr->pasteboardID_cstr              = NULL;

	pbptr->flags.reserved                 = 0U;
	pbptr->flags.phase                    = global_options;
	pbptr->flags.has_args                 = false;
}

int parsearg(const char *arg, struct argblock *pbptr) {
	const char *param;
	switch(pbptr->flags.phase) {
		case global_options:
			if(testarg(arg, "--type=", &param)) {
				//Don't use create_UTI_with_cstr here because the user should be able to explicitly request a type that might not have been declared.
				pbptr->type = CFStringCreateWithCString(kCFAllocatorDefault, param, kCFStringEncodingUTF8);
			} else if(testarg(arg, "--pasteboard=", &param)) {
				pbptr->pasteboardID_cstr = param;
				pbptr->pasteboardID = CFStringCreateWithCString(kCFAllocatorDefault, param, kCFStringEncodingUTF8);
			} else if(testarg(arg, "--in-file=", &param)) {
				pbptr->in_fd = open(param, O_RDONLY, 0644);
				if(pbptr->in_fd != -1)
					pbptr->filename = param;
			} else if(testarg(arg, "--out-file=", &param)) {
				pbptr->out_fd = open(param, O_WRONLY | O_CREAT, 0644);
				if(pbptr->out_fd != -1)
					pbptr->filename = param;
			} else if(testarg(arg, "copy", NULL)
				 || testarg(arg, "paste", NULL)
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

static Boolean copy_type_by_filename(struct argblock *pbptr) {
	Boolean success = false;

	if(pbptr->filename) {
		FSRef ref;
		Boolean isDir;
		OSStatus err = FSPathMakeRef((const UInt8 *)(pbptr->filename), &ref, &isDir);
		if (err == noErr)
			err = LSCopyItemAttribute(&ref, kLSRolesAll, kLSItemContentType, (CFTypeRef *)&(pbptr->type));

		if(!(pbptr->type)) {
			//Presumably, the file doesn't exist (FSPathMakeRef failed). Let's try breaking off the filename extension and looking it up.
			CFStringRef pathCF = CFStringCreateWithCString(kCFAllocatorDefault, pbptr->filename, kCFStringEncodingUTF8);
			if(pathCF) {
				CFIndex length = CFStringGetLength(pathCF);
				UniChar *buffer = malloc(length * sizeof(UniChar));
				if(buffer) {
					CFStringGetCharacters(pathCF, (CFRange){ 0, length }, buffer);

					UniCharCount startIndex = 0U;
					err = LSGetExtensionInfo(length, buffer, &startIndex);
					if(err == noErr) {
						CFStringRef extension = CFStringCreateWithSubstring(kCFAllocatorDefault, pathCF, (CFRange){ startIndex, length - startIndex });

						//Here's where the actual look-up occurs.
						if(extension) {
							pbptr->type = UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, extension, /*conformingToUTI*/ NULL);

							CFRelease(extension);
						}
					}

					free(buffer);
				}

				CFRelease(pathCF);
			}
		}

		if(pbptr->type) {
			//Don't allow kUTTypePlainText (.txt). If we get this, just return NULL.
			if(UTTypeEqual(pbptr->type, kUTTypePlainText)) {
				CFRelease(pbptr->type);
				pbptr->type = NULL;
			}
		}

		success = (pbptr->type != NULL);
	}

	return success;
}

static void null_deallocator(const char *ptr) {
}

static const char *make_cstr_for_CFStr(CFStringRef in, CFStringEncoding encoding, void (**outDeallocator)(const char *ptr)) {
	const char *result = NULL;
	void (*deallocator)(const char *ptr) = null_deallocator;
	if(in) {
		result = CFStringGetCStringPtr(in, encoding);
		if(result == NULL) {
			CFRange IDrange = CFRangeMake(0, CFStringGetLength(in));
			CFIndex numBytes = 0;
			CFStringGetBytes(in, IDrange, encoding, /*lossByte*/ 0U, /*isExternalRepresentation*/ false, /*buffer*/ NULL, /*maxBufLen*/ 0, &numBytes);
			char *buf = pb_allocate(numBytes + 1U);
			if(buf) {
				CFIndex numChars __attribute__((unused)) = CFStringGetBytes(in, IDrange, encoding, /*lossByte*/ 0U, /*isExternalRepresentation*/ false, (unsigned char *)buf, /*maxBufLen*/ numBytes, &numBytes);
				buf[numBytes] = 0;
				deallocator = (void (*)(const char *ptr))pb_deallocate;
			}
			result = buf;
		}
	}
	if(outDeallocator) {
		*outDeallocator = deallocator;
	}
	return result;
}
static const char *make_pasteboardID_cstr(struct argblock *pbptr, void(**outDeallocator)(const char *ptr)) {
	if(pbptr->pasteboardID_cstr == NULL)
		pbptr->pasteboardID_cstr = make_cstr_for_CFStr(pbptr->pasteboardID, kCFStringEncodingUTF8, outDeallocator);
	return pbptr->pasteboardID_cstr;
}

static PasteboardItemID getRandomPasteboardItemID(void) {
	//Item IDs are determined by the application creating the item.
	//Their meaning is up to that same application.
	//pb doesn't need to associate any meaning with the item, so we just pull
	//  the ID out of a hat.

	PasteboardItemID item;

	item = (PasteboardItemID)(unsigned long)arc4random();

	//An item ID of 0 is illegal. Make sure it doesn't happen.
	if(item == 0)
		++item;

	return item;
}

#pragma mark -

int copy(struct argblock *pbptr) {
#	define CONSUME_ARG                                                                                   \
		if(pbptr->argc) {                                                                                 \
			/*If we don't already have an explicit type, try to get one. Otherwise, just get a filename.*/ \
			CFStringRef UTI = pbptr->type ? NULL : create_UTI_with_cstr(*(pbptr->argv));                    \
			if(UTI)                                                                                          \
				pbptr->type = UTI;                                                                            \
			else {                                                                                             \
				/*This is a filename.*/                                                                         \
				pbptr->in_fd = open(*(pbptr->argv), O_RDONLY, 0644);                                             \
				if(pbptr->in_fd != -1)                                                                            \
					pbptr->filename = *(pbptr->argv);                                                              \
                                                                                                                    \
			}                                                                                                        \
			++(pbptr->argv); --(pbptr->argc);                                                                         \
		}
	CONSUME_ARG
	CONSUME_ARG
#	undef CONSUME_ARG

	char *buf = NULL;
	size_t total_size = 0U;

	enum { increment = 1048576U };
	ssize_t amt_read = 0;
	size_t bufsize = 0U;
	do {
		if(total_size % increment == 0U)
			buf = realloc(buf, bufsize += increment);
		total_size += amt_read = read(pbptr->in_fd, &buf[total_size], bufsize - (total_size % increment));
	} while(amt_read);

	OSStatus err;
	int retval = 0;

	err = PasteboardClear(pbptr->pasteboard);
	if(err != noErr) {
		fprintf(stderr, "%s copy: could not clear pasteboard %s because PasteboardClear returned %li (%s)\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
		return 2;
	}

	PasteboardItemID item = getRandomPasteboardItemID();

	CFStringRef string;
	CFDataRef data = NULL;
	if(pbptr->type == NULL) {
		//Try to get a type by filename extension.
		if(!copy_type_by_filename(pbptr)) {
			//We couldn't figure out a type, so let's see whether it's valid UTF-8.
			string = CFStringCreateWithBytes(kCFAllocatorDefault, (const unsigned char *)buf, total_size, kCFStringEncodingUTF8, /*isExternalRepresentation*/ false);
			if(string != NULL) {
				pbptr->type = CFRetain(kUTTypeUTF8PlainText);

				//We don't actually use the string for anything.
				CFRelease(string);
			} else {
				//Apparently not. Our best guess is to call it MacRoman and copy the pure bytes.
				pbptr->type = CFRetain(MacRoman_UTI);
				goto pure_data;
			}

			data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)buf, total_size);
		}
	}
	if(!data) {
pure_data:
		data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, (const unsigned char *)buf, total_size, /*bytesDeallocator*/ kCFAllocatorNull);
		if(data == NULL)
			data = CFDataCreate(kCFAllocatorDefault, (const unsigned char *)buf, total_size);
		if(data == NULL) {
			fprintf(stderr, "%s copy: could not create CFData object for copy to pasteboard %s\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL));
			return 2;
		}
	}

	//Always do this first.
	err = PasteboardPutItemFlavor(pbptr->pasteboard, item, pbptr->type, data, kPasteboardFlavorNoFlags);
	if(err != noErr) {
		fprintf(stderr, "%s copy: could not copy data for main type \"%s\": PasteboardPutItemFlavor returned error %li (%s)\n", argv0, make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
	}

	//Translate encodings.
	CFDataRef UTF16Data = NULL, UTF16ExtData = NULL, UTF8Data = NULL, MacRomanData = NULL;
	Boolean typeIsUTF16 = false, typeIsUTF16Ext = false, typeIsUTF8 = false, typeIsMacRoman = false;
	Boolean isTextData = false; //Note: We can't just test conformance to public.text because that includes formats like public.rtf.
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
	if(isTextData) {
		convert_encodings(&UTF16Data, &UTF16ExtData, &UTF8Data, &MacRomanData);
		//Only copy it if we did not already copy it (which we have done if its type is pbptr->type).
		if(UTF16Data && !typeIsUTF16) {
			err = PasteboardPutItemFlavor(pbptr->pasteboard, item, kUTTypeUTF16PlainText, UTF16Data, kPasteboardFlavorSenderTranslated);
			if(err != noErr) {
				fprintf(stderr, "%s copy: could not copy alternate \"%s\" data for main type \"%s\": PasteboardPutItemFlavor returned error %li (%s)\n", argv0, make_cstr_for_CFStr(kUTTypeUTF16PlainText, kCFStringEncodingUTF8, /*deallocator*/ NULL), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
				err = noErr; //These aren't critically-important.
			}
		}
		if(UTF16ExtData && !typeIsUTF16Ext) {
			err = PasteboardPutItemFlavor(pbptr->pasteboard, item, kUTTypeUTF16ExternalPlainText, UTF16ExtData, kPasteboardFlavorSenderTranslated);
			if(err != noErr) {
				fprintf(stderr, "%s copy: could not copy alternate \"%s\" data for main type \"%s\": PasteboardPutItemFlavor returned error %li (%s)\n", argv0, make_cstr_for_CFStr(kUTTypeUTF16ExternalPlainText, kCFStringEncodingUTF8, /*deallocator*/ NULL), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
				err = noErr; //These aren't critically-important.
			}
		}
		if(UTF8Data && !typeIsUTF8) {
			err = PasteboardPutItemFlavor(pbptr->pasteboard, item, kUTTypeUTF8PlainText, UTF8Data, kPasteboardFlavorSenderTranslated);
			if(err != noErr) {
				fprintf(stderr, "%s copy: could not copy alternate \"%s\" data for main type \"%s\": PasteboardPutItemFlavor returned error %li (%s)\n", argv0, make_cstr_for_CFStr(kUTTypeUTF8PlainText, kCFStringEncodingUTF8, /*deallocator*/ NULL), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
				err = noErr; //These aren't critically-important.
			}
		}
		if(MacRomanData && !typeIsMacRoman) {
			err = PasteboardPutItemFlavor(pbptr->pasteboard, item, MacRoman_UTI, MacRomanData, kPasteboardFlavorSenderTranslated);
			if(err != noErr) {
				fprintf(stderr, "%s copy: could not copy alternate \"%s\" data for main type \"%s\": PasteboardPutItemFlavor returned error %li (%s)\n", argv0, make_cstr_for_CFStr(MacRoman_UTI, kCFStringEncodingUTF8, /*deallocator*/ NULL), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
				err = noErr; //These aren't critically-important.
			}
		}
	}

	CFRelease(data);
	free(buf);

	if(err != noErr) {
		fprintf(stderr, "%s copy: could not copy to pasteboard %s because PasteboardPutItemFlavor returned %li (%s)\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
		retval = 2;
	}

	return retval;
}
int paste_one(struct argblock *pbptr) {
	int retval = 0;
	OSStatus err;

	PasteboardItemID item;
	err = PasteboardGetItemIdentifier(pbptr->pasteboard, pbptr->itemIndex, &item);
	if(err != noErr) {
		fprintf(stderr, "%s: can't find item %lu on pasteboard %s: PasteboardGetItemIdentifier returned %li (%s)\n", argv0, (unsigned long)pbptr->itemIndex, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
		return 2;
	}

	CFDataRef data = NULL;
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
			fprintf(stderr, "%s: could not paste item %lu of pasteboard \"%s\": it does not exist in flavor type \"%s\".\n", argv0, (unsigned long)pbptr->itemIndex, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8, /*deallocator*/ NULL));
		else
			fprintf(stderr, "%s: could not paste item %lu of pasteboard \"%s\": PasteboardCopyItemFlavorData (for flavor type \"%s\") returned error %li (%s)\n", argv0, (unsigned long)pbptr->itemIndex, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), make_cstr_for_CFStr(pbptr->type, kCFStringEncodingUTF8, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
		retval = 2;
	} else {
		write(pbptr->out_fd, CFDataGetBytePtr(data), CFDataGetLength(data));
	}

	if(data)
		CFRelease(data);

	return retval;
}
int paste(struct argblock *pbptr) {
	ItemCount numItems = 0U;
	OSStatus err = PasteboardGetItemCount(pbptr->pasteboard, &numItems);
	if(err != noErr) {
		fprintf(stderr, "%s: could not determine how many items are on pasteboard %s: PasteboardGetItemCount returned %li (%s)\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
		return 2;
	}

	if(!(pbptr->argc)) {
		if((pbptr->out_fd) < 0)
			pbptr->out_fd = STDOUT_FILENO;
		if(!(pbptr->type))
			pbptr->type = CFRetain(kUTTypeUTF8PlainText);
		if((pbptr->itemIndex) == 0UL)
			pbptr->itemIndex = 1UL;
		return paste_one(pbptr);
	} else {
		struct argblock  these_args     = *pbptr;
		struct argblock *these_args_ptr = &these_args;

		const char *type_cstr = NULL;
		CFStringRef type = NULL;
		const char *index_cstr = NULL;
		while(*(pbptr->argv)) {
			//Any options provided before the paste command are the default values for options after the paste command. If we encounter another value on the command line, use that.
			//If two option values after the paste command collide (e.g. two filenames), paste_one is invoked with the first value, and then we will begin a new set of arguments with the second value.
			//If we get all three option values (filename, type, index), paste_one is invoked, and then we begin a new set of arguments.
			Boolean has_encountered_index = false;
			CFIndex numericValue;

			while(*(pbptr->argv) && !(pbptr->filename && type_cstr && index_cstr)) {
				const char *option_arg = NULL;

				if(compare_argument('f', "file", pbptr->argv, &pbptr->argv, /*out_args_consumed*/ NULL, /*option_arg_optional*/ false, &option_arg) & option_comparison_eitheropt) {
					if(!(pbptr->filename))
						pbptr->filename = option_arg;
					else
						break;
				} else if(compare_argument('t', "type", pbptr->argv, &pbptr->argv, /*out_args_consumed*/ NULL, /*option_arg_optional*/ false, &option_arg) & option_comparison_eitheropt) {
					if(!pbptr->type) {
						//Don't use create_UTI_with_cstr here because the user should be able to explicitly request a type that might not have been declared.
						pbptr->type = CFStringCreateWithCString(kCFAllocatorDefault, option_arg, kCFStringEncodingUTF8);
					} else
						break;
				} else if(compare_argument('i', "index", pbptr->argv, &pbptr->argv, /*out_args_consumed*/ NULL, /*option_arg_optional*/ false, &option_arg) & option_comparison_eitheropt) {
					if(!index_cstr) {
						index_cstr = option_arg;
						numericValue = strtoul(index_cstr, NULL, 10);
						if(numericValue == 0U) {
							fprintf(stderr, "%s: Invalid index %lu\n", argv0, numericValue);
							return 1;
						} else if(numericValue > numItems) {
							fprintf(stderr, "%s: Index %lu exceeds number of items %lu\n", argv0, (unsigned long)numericValue, numItems);
							return 1;
						} else {
							if(has_encountered_index)
								break;
							else {
								pbptr->itemIndex = numericValue;
								has_encountered_index = true;
							}
						}
					} else
						break;
				} else if(*(*(pbptr->argv)) == '-') {
					fprintf(stderr, "%s: Internal error: Unrecognized argument “%s”\n", argv0, *(pbptr->argv));
					return 1;
				} else {
					numericValue = strtoul(*(pbptr->argv), NULL, 10);
					if((numericValue > 0U) && (numericValue < numItems)) {
						if(has_encountered_index)
							break;
						else {
							pbptr->itemIndex = numericValue;
							index_cstr = *((pbptr->argv)++);
							has_encountered_index = true;
						}
					} else if((numericValue == 0U) && (type = create_UTI_with_cstr(*(pbptr->argv)))) {
						if(pbptr->type)
							break;
						else {
							type_cstr = *((pbptr->argv)++);
							pbptr->type = type;
						}
					} else {
						if(pbptr->filename)
							break;
						else
							pbptr->filename = *((pbptr->argv)++);
					}
				}
			}

			these_args = *pbptr;
			if(these_args_ptr->out_fd < 0)
				these_args_ptr->out_fd    = pbptr->filename ? open(pbptr->filename, O_WRONLY | O_CREAT | O_TRUNC, 0644) : STDOUT_FILENO;
			if(!these_args_ptr->type)
				these_args_ptr->type      = kUTTypeUTF8PlainText; //Don't retain; borrow the retention by pbptr.
			if(!these_args_ptr->itemIndex)
				these_args_ptr->itemIndex = 1U;

			int status = paste_one(these_args_ptr);
			if(status != 0)
				return status;

			if((pbptr->out_fd >= 0) && (pbptr->out_fd != STDOUT_FILENO)) {
				close(pbptr->out_fd);
				pbptr->out_fd = -1;
			}
			pbptr->filename = NULL;
			pbptr->type = NULL;
			++pbptr->itemIndex;
		}
	}

	return 0;
}
int count(struct argblock *pbptr) {
	ItemCount num;
	OSStatus err = PasteboardGetItemCount(pbptr->pasteboard, &num);
	if(err != noErr) {
		fprintf(stderr, "%s count: PasteboardGetItemCount for pasteboard %s returned %li (%s)\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
		return 2;
	}
	printf("%lu\n", (unsigned long)num);
	return 0;
}
int list(struct argblock *pbptr) {
	bool showSizes = false;
	if (pbptr->argc > 0) {
		if (compare_argument('s', "show-sizes", pbptr->argv, &pbptr->argv, /*out_args_consumed*/ NULL, /*option_arg_optional*/ false, /*out_option_arg*/ NULL) & option_comparison_eitheropt) {
			showSizes = true;
		}
	}
	ItemCount num;
	OSStatus err = PasteboardGetItemCount(pbptr->pasteboard, &num);
	if(err != noErr) {
		fprintf(stderr, "%s list: PasteboardGetItemCount for pasteboard %s returned %li (%s)\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
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
				fprintf(stderr, "%s list: PasteboardGetItemIdentifier for pasteboard %s item %lu returned %li (%s)\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (unsigned long)i, (long)err, GetMacOSStatusCommentString(err));
				break;
			}

			err = PasteboardCopyItemFlavors(pbptr->pasteboard, item, &flavors);
			if(err != noErr) {
				fprintf(stderr, "%s list: PasteboardCopyItemFlavors for pasteboard %s item %lu (object address %p) returned %li (%s)\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (unsigned long)i, item, (long)err, GetMacOSStatusCommentString(err));
				break;
			}

			CFIndex numFlavors = CFArrayGetCount(flavors);
			printf("\n#%u: %lu flavors\n", i, numFlavors);
			for(CFIndex j = 0U; j < numFlavors; ++j) {
				CFStringRef flavor = CFArrayGetValueAtIndex(flavors, j);
				void (*flavor_deallocator)(const char *ptr) = null_deallocator;
				const char *flavor_c = make_cstr_for_CFStr(flavor, kCFStringEncodingUTF8, &flavor_deallocator);
				CFStringRef tag = UTTypeCopyPreferredTagWithClass(flavor, kUTTagClassOSType);
				void (*tag_deallocator)(const char *ptr) = null_deallocator;
				const char *tag_c = tag ? make_cstr_for_CFStr(tag, kCFStringEncodingUTF8, &tag_deallocator) : NULL;

				CFDataRef flavorData = NULL;
				if (showSizes) {
					err = PasteboardCopyItemFlavorData(pbptr->pasteboard, item, flavor, &flavorData);
				}

				printf("\t%s ", flavor_c);
				if (tag_c != NULL && *tag_c != '\0') {
					printf("'%s' ", tag_c);
				}
				if (showSizes) {
					if(err == noErr) {
						printf("(%lli bytes)\n", (long long)CFDataGetLength(flavorData));
						CFRelease(flavorData);
					} else {
						printf("(??? bytes; PasteboardCopyItemFlavorData returned %li (%s))\n", (long)err, GetMacOSStatusCommentString(err));
					}
				} else {
					printf("\n");
				}
				//else...
				//	printf("\t%s\n", flavor_c);
				flavor_deallocator(flavor_c);
				tag_deallocator(tag_c);
			}
		}
	}

	return 0;
}
int clear(struct argblock *pbptr) {
	OSStatus err = PasteboardClear(pbptr->pasteboard);
	if(err != noErr) {
		fprintf(stderr, "%s clear: PasteboardClear for pasteboard %s returned %li (%s)\n", argv0, make_pasteboardID_cstr(pbptr, /*deallocator*/ NULL), (long)err, GetMacOSStatusCommentString(err));
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
	printf("pb version 1.0b1\n"
		   "Copyright 2009 Peter Hosey\n"
		   "Interface to the Pasteboard Manager:\n"
		   "Read and write pasteboards (including the clipboard)\n"
		   "\n"
		   "Type %s help for usage.\n", argv0);
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

	if(firstAllocation == NULL) {
		//No allocations in the list; just free it.
		free(buf);
	} else if(firstAllocation->ptr == buf) {
		if(firstAllocation == lastAllocation)
			lastAllocation = NULL;
		//If we set lastAllocation to NULL above, this should set firstAllocation to NULL; otherwise they should both be non-NULL.
		firstAllocation = allocation->next;
		free(buf);
		free(allocation);
	} else {
		//We now know the first allocation isn't it. Find one in the rest of the list that matches, and unlink it.
		while(allocation != NULL) {
			struct allocation *nextAllocation = allocation->next;
			if(nextAllocation != NULL) {
				if(nextAllocation->ptr == buf) {
					//Gotcha. Unlink this element from the list.
					allocation->next = nextAllocation->next;
					if(nextAllocation == lastAllocation)
						lastAllocation = NULL;
					free(nextAllocation);
					free(buf);
				}
			}
			allocation = nextAllocation;
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
				string = CFStringCreateWithCharactersNoCopy(kCFAllocatorDefault,
			                                      	  (const UniChar *)CFDataGetBytePtr(*inoutUTF16Data),
			                                      	  CFDataGetLength(*inoutUTF16Data) / sizeof(UniChar),
													  kCFAllocatorNull);
				if(!string)
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
				string = CFStringCreateWithCharactersNoCopy(kCFAllocatorDefault,
			                                      	  (const UniChar *)CFDataGetBytePtr(*inoutUTF16Data),
			                                      	  CFDataGetLength(*inoutUTF16Data) / sizeof(UniChar),
													  kCFAllocatorNull);
				if(!string)
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
				string = CFStringCreateWithCharactersNoCopy(kCFAllocatorDefault,
			                                      	  (const UniChar *)CFDataGetBytePtr(*inoutUTF16Data),
			                                      	  CFDataGetLength(*inoutUTF16Data) / sizeof(UniChar),
													  kCFAllocatorNull);
				if(!string)
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

static CFStringRef create_UTI_with_cstr(const char *arg) {
	Boolean isUTI = false;

	CFStringRef possibleUTI = CFStringCreateWithCString(kCFAllocatorDefault, arg, kCFStringEncodingUTF8);
	if(possibleUTI) {
		CFStringRef tag = UTTypeCopyPreferredTagWithClass(possibleUTI, kUTTagClassFilenameExtension);
		if(tag)
			isUTI = true;
		else {
			tag = UTTypeCopyPreferredTagWithClass(possibleUTI, kUTTagClassOSType);
			if(tag)
				isUTI = true;
			else {
				tag = UTTypeCopyPreferredTagWithClass(possibleUTI, kUTTagClassNSPboardType);
				if(tag)
					isUTI = true;
				else {
					tag = UTTypeCopyPreferredTagWithClass(possibleUTI, kUTTagClassMIMEType);
					if(tag)
						isUTI = true;
				}
			}
		}

		if(!isUTI) {
			CFRelease(possibleUTI);
			possibleUTI = NULL;
		}
	}

	return possibleUTI;
}
