#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "folio_macos.h"
#include <string.h>

char *macos_state_directory(void)
{
    @autoreleasepool {
        NSFileManager *files = [NSFileManager defaultManager];
        NSURL *support = [files URLForDirectory:NSApplicationSupportDirectory
                                      inDomain:NSUserDomainMask
                             appropriateForURL:nil create:YES error:NULL];
        if (support == nil) return NULL;
        NSURL *directory = [support URLByAppendingPathComponent:@"Folio" isDirectory:YES];
        if (![files createDirectoryAtURL:directory withIntermediateDirectories:YES
                              attributes:nil error:NULL]) return NULL;
        return strdup(directory.fileSystemRepresentation);
    }
}

int macos_choose_file(char *path, size_t size)
{
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.title = @"Open PDF";
        panel.allowedContentTypes = @[UTTypePDF];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if ([panel runModal] != NSModalResponseOK) return 0;
        const char *selected = panel.URL.fileSystemRepresentation;
        if (selected == NULL || strlen(selected) >= size) return -1;
        memcpy(path, selected, strlen(selected) + 1);
        return 1;
    }
}

#import <PDFKit/PDFKit.h>
#import <objc/runtime.h>
#include "../../src/platform.h"

static NSWindow *reader_window(void)
{
    for(NSWindow *window in NSApp.windows)
        if([NSStringFromClass(window.class) containsString:@"GLFWWindow"]) return window;
    return NSApp.mainWindow;
}

@interface FolioAXElement : NSAccessibilityElement
@property unsigned commandID;
@end
@implementation FolioAXElement
- (BOOL)accessibilityPerformPress {
    if(!self.commandID||!self.accessibilityEnabled) return NO;
    platform_deliver_command(PLATFORM_ACCESSIBILITY,self.commandID);return YES;
}
@end

@interface FolioAXView : NSView
@property(strong) NSArray<FolioAXElement *> *readerElements;
@property BOOL readerScrollable;
@property(strong) NSMutableDictionary<NSNumber *,FolioAXElement *> *cache;
@end
@implementation FolioAXView
- (NSView *)hitTest:(NSPoint)point { (void)point;return nil; }
- (BOOL)isAccessibilityElement { return YES; }
- (NSString *)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString *)accessibilityLabel { return @"PDF reader"; }
- (NSArray *)accessibilityChildren { return self.readerElements ? self.readerElements : @[]; }
- (BOOL)accessibilityPerformScrollDown {if(!self.readerScrollable) return NO;platform_deliver_command(PLATFORM_SCROLL_DOWN,0);return YES;}
- (BOOL)accessibilityPerformScrollUp {if(!self.readerScrollable) return NO;platform_deliver_command(PLATFORM_SCROLL_UP,0);return YES;}
- (id)accessibilityHitTest:(NSPoint)point {
    for(FolioAXElement *element in self.readerElements.reverseObjectEnumerator)
        if(NSPointInRect(point,element.accessibilityFrame)) return element;
    return self;
}
@end

@interface FolioBridge : NSObject
@property(strong) FolioAXView *accessibilityView;
@property(strong) NSSharingServicePicker *sharingPicker;
@property BOOL documentReady, copyAllowed, printAllowed, selectionActive;
@end
@implementation FolioBridge
- (void)command:(NSMenuItem *)item {
    id responder=NSApp.keyWindow.firstResponder;
    if([responder isKindOfClass:NSTextView.class]&&(item.tag==PLATFORM_COPY||item.tag==PLATFORM_SELECT_ALL)) {
        [NSApp sendAction:item.tag==PLATFORM_COPY?@selector(copy:):@selector(selectAll:) to:nil from:item];return;
    }
    platform_deliver_command((PlatformCommand)item.tag,0);
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if(item.action!=@selector(command:)) return YES;
    if(item.tag==PLATFORM_OPEN) return YES;
    if([NSApp.keyWindow.firstResponder isKindOfClass:NSTextView.class]&&(item.tag==PLATFORM_COPY||item.tag==PLATFORM_SELECT_ALL)) return YES;
    if(item.tag==PLATFORM_PRINT) return self.documentReady&&self.printAllowed;
    if(item.tag==PLATFORM_COPY) return self.documentReady&&self.copyAllowed;
    if(item.tag==PLATFORM_SELECT_ALL) return self.documentReady&&self.copyAllowed;
    return self.documentReady;
}
@end
static FolioBridge *bridge;

static void open_files(id delegate,SEL selector,NSApplication *application,NSArray<NSString *> *paths)
{
    (void)delegate;(void)selector;
    for(NSString *path in paths) platform_deliver_path(path.fileSystemRepresentation);
    [application replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

void macos_prepare(void)
{
    bridge=[FolioBridge new];
    // GLFW owns the application delegate and dispatches the initial Finder event
    // inside InitWindow. Supply its optional document callback before that happens.
    class_addMethod(NSClassFromString(@"GLFWApplicationDelegate"),@selector(application:openFiles:),
                    (IMP)open_files,"v@:@@");
}

static void menu_item(NSMenu *menu,NSString *title,NSString *key,PlatformCommand command,NSEventModifierFlags flags)
{
    NSMenuItem *item=[[NSMenuItem alloc] initWithTitle:title action:@selector(command:) keyEquivalent:key];
    item.target=bridge;item.tag=command;item.keyEquivalentModifierMask=flags;
    [menu addItem:item];
}

void macos_init(void)
{
    NSMenu *main=NSApp.mainMenu;
    NSMenuItem *file=[[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    NSMenu *files=[[NSMenu alloc] initWithTitle:@"File"];file.submenu=files;
    [main insertItem:file atIndex:(main.numberOfItems<1?main.numberOfItems:1)];
    menu_item(files,@"Open PDF…",@"o",PLATFORM_OPEN,NSEventModifierFlagCommand);
    menu_item(files,@"Save a Copy…",@"s",PLATFORM_EXPORT,NSEventModifierFlagCommand|NSEventModifierFlagShift);
    menu_item(files,@"Share…",@"",PLATFORM_SHARE,0);
    [files addItem:NSMenuItem.separatorItem];
    menu_item(files,@"Print…",@"p",PLATFORM_PRINT,NSEventModifierFlagCommand);
    NSMenuItem *close=[[NSMenuItem alloc] initWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
    [files addItem:close];
    NSMenuItem *edit=[[NSMenuItem alloc] initWithTitle:@"Edit" action:nil keyEquivalent:@""];
    NSMenu *edits=[[NSMenu alloc] initWithTitle:@"Edit"];edit.submenu=edits;
    [main insertItem:edit atIndex:(main.numberOfItems<2?main.numberOfItems:2)];
    menu_item(edits,@"Copy",@"c",PLATFORM_COPY,NSEventModifierFlagCommand);
    menu_item(edits,@"Select All Text on Page",@"a",PLATFORM_SELECT_ALL,NSEventModifierFlagCommand);
    [edits addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edits addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edits addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    [edits addItem:NSMenuItem.separatorItem];
    menu_item(edits,@"Find…",@"f",PLATFORM_FIND,NSEventModifierFlagCommand);
    menu_item(edits,@"Find Next",@"g",PLATFORM_FIND_NEXT,NSEventModifierFlagCommand);
    menu_item(edits,@"Find Previous",@"g",PLATFORM_FIND_PREVIOUS,NSEventModifierFlagCommand|NSEventModifierFlagShift);
    NSMenuItem *view=[[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
    NSMenu *views=[[NSMenu alloc] initWithTitle:@"View"];view.submenu=views;
    [main insertItem:view atIndex:(main.numberOfItems<3?main.numberOfItems:3)];
    menu_item(views,@"Go to Page…",@"l",PLATFORM_GO_PAGE,NSEventModifierFlagCommand);
    menu_item(views,@"Zoom In",@"+",PLATFORM_ZOOM_IN,NSEventModifierFlagCommand);
    menu_item(views,@"Zoom Out",@"-",PLATFORM_ZOOM_OUT,NSEventModifierFlagCommand);
    menu_item(views,@"Fit Page",@"0",PLATFORM_FIT,NSEventModifierFlagCommand);
    [views addItem:NSMenuItem.separatorItem];
    menu_item(views,@"Back",@"[",PLATFORM_BACK,NSEventModifierFlagCommand);
    menu_item(views,@"Forward",@"]",PLATFORM_FORWARD,NSEventModifierFlagCommand);
}

bool macos_request_text(const char *label,const char *initial,int input)
{
    NSWindow *window=reader_window();
    if(!window) return false;
    NSAlert *alert=[NSAlert new];
    alert.messageText=[NSString stringWithUTF8String:label];
    [alert addButtonWithTitle:@"OK"];[alert addButtonWithTitle:@"Cancel"];
    NSTextField *field=input==2?[[NSSecureTextField alloc] initWithFrame:NSMakeRect(0,0,340,26)]:
                               [[NSTextField alloc] initWithFrame:NSMakeRect(0,0,340,26)];
    field.stringValue=[NSString stringWithUTF8String:initial];
    alert.accessoryView=field;
    [alert beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        platform_deliver_text(field.stringValue.UTF8String,response==NSAlertFirstButtonReturn);
    }];
    [alert.window makeFirstResponder:field];[field selectText:nil];
    return true;
}

bool macos_copy_text(const char *text)
{
    NSString *value=[NSString stringWithUTF8String:text];
    if(!value) return false;
    [NSPasteboard.generalPasteboard clearContents];
    return [NSPasteboard.generalPasteboard setString:value forType:NSPasteboardTypeString];
}

bool macos_open_link(const char *uri)
{
    NSURL *url=[NSURL URLWithString:[NSString stringWithUTF8String:uri]];
    return url&&[NSWorkspace.sharedWorkspace openURL:url];
}

bool macos_document_action(int command,const char *path,const char *title,const char *password)
{
    NSWindow *window=reader_window();
    if(!window) return false;
    NSURL *source=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    NSString *name=[NSString stringWithUTF8String:title];
    if(command==PLATFORM_EXPORT) {
        NSSavePanel *panel=[NSSavePanel savePanel];panel.allowedContentTypes=@[UTTypePDF];
        panel.nameFieldStringValue=name;
        [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse result) {
            if(result!=NSModalResponseOK) return;
            if([source.path isEqualToString:panel.URL.path]) {platform_notice("Choose a different name for the copy.");return;}
            // The save panel already asks before replacing an existing destination.
            NSURL *destination=panel.URL;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
                NSError *writeError=nil;
                NSData *bytes=[NSData dataWithContentsOfURL:source options:NSDataReadingMappedIfSafe error:&writeError];
                if(!bytes||![bytes writeToURL:destination options:NSDataWritingAtomic error:&writeError])
                    platform_notice("Could not save the PDF copy.");
                else platform_notice("Saved PDF copy.");
            });
        }];return true;
    }
    if(command==PLATFORM_SHARE) {
        bridge.sharingPicker=[[NSSharingServicePicker alloc] initWithItems:@[source]];
        [bridge.sharingPicker showRelativeToRect:NSMakeRect(window.contentView.bounds.size.width-80,window.contentView.bounds.size.height-30,1,1)
                                         ofView:window.contentView preferredEdge:NSMinYEdge];
        return true;
    }
    if(command==PLATFORM_PRINT) {
        PDFDocument *document=[[PDFDocument alloc] initWithURL:source];
        if(document.isLocked) [document unlockWithPassword:[NSString stringWithUTF8String:password]];
        if(!document||document.isLocked) {platform_notice("Could not prepare this PDF for printing.");return false;}
        NSPrintInfo *info=[NSPrintInfo.sharedPrintInfo copy];
        NSPrintOperation *operation=[document printOperationForPrintInfo:info scalingMode:kPDFPrintPageScaleDownToFit autoRotate:YES];
        operation.jobTitle=name;
        [operation runOperationModalForWindow:window delegate:nil didRunSelector:NULL contextInfo:NULL];
        return true;
    }
    return false;
}

void macos_accessibility(const char *json)
{
    NSWindow *window=reader_window();if(!window) return;
    NSData *bytes=[[NSString stringWithUTF8String:json] dataUsingEncoding:NSUTF8StringEncoding];
    if(!bytes) return;
    NSDictionary *snapshot=[NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];
    if(!snapshot) return;
    if(!bridge.accessibilityView||bridge.accessibilityView.window!=window) {
        [bridge.accessibilityView removeFromSuperview];
        FolioAXView *view=[[FolioAXView alloc] initWithFrame:window.contentView.bounds];
        view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
        view.cache=[NSMutableDictionary dictionary];
        [window.contentView addSubview:view];bridge.accessibilityView=view;
    }
    FolioAXView *view=bridge.accessibilityView;
    view.readerScrollable=[snapshot[@"scrollable"] boolValue];
    bridge.documentReady=[snapshot[@"hasDocument"] boolValue];
    bridge.copyAllowed=[snapshot[@"canCopy"] boolValue];bridge.printAllowed=[snapshot[@"canPrint"] boolValue];
    bridge.selectionActive=[snapshot[@"selection"] boolValue];
    NSMutableArray *children=[NSMutableArray array];
    NSMutableDictionary *next=[NSMutableDictionary dictionary];
    NSMutableArray *nodes=[snapshot[@"nodes"] mutableCopy];
    NSString *text=snapshot[@"document"];
    if(text.length) {
        NSArray *b=snapshot[@"body"];
        [nodes addObject:@{@"id":@0,@"x":b[0],@"y":b[1],@"w":b[2],@"h":b[3],@"label":text,@"text":@YES,@"enabled":@YES}];
    }
    CGFloat height=view.bounds.size.height;
    for(NSDictionary *node in nodes) {
        NSNumber *identifier=node[@"id"];
        FolioAXElement *element=view.cache[identifier];
        if(!element) element=[FolioAXElement new];
        BOOL isText=[node[@"text"] boolValue];
        BOOL textChanged=isText&&element.accessibilityValue&&![element.accessibilityValue isEqual:node[@"label"]];
        element.accessibilityParent=view;
        element.accessibilityRole=isText?NSAccessibilityStaticTextRole:NSAccessibilityButtonRole;
        element.accessibilityLabel=isText?@"Page text":node[@"label"];
        element.accessibilityValue=isText?node[@"label"]:([node[@"selected"] boolValue]?@"Selected":nil);
        element.accessibilityEnabled=[node[@"enabled"] boolValue];
        element.commandID=isText?0:identifier.unsignedIntValue;
        NSRect box=NSMakeRect([node[@"x"] doubleValue],height-[node[@"y"] doubleValue]-[node[@"h"] doubleValue],
                              [node[@"w"] doubleValue],[node[@"h"] doubleValue]);
        element.accessibilityFrame=[window convertRectToScreen:[view convertRect:box toView:nil]];
        [children addObject:element];next[identifier]=element;
        if(textChanged) NSAccessibilityPostNotification(element,NSAccessibilityValueChangedNotification);
    }
    BOOL changed=![view.readerElements isEqualToArray:children];
    view.cache=next;view.readerElements=children;
    if(changed) NSAccessibilityPostNotification(view,NSAccessibilityLayoutChangedNotification);
}
