/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#include "nsWidget.h"

#include "nsIServiceManager.h"

#include "nsAppShell.h"

#include <X11/cursorfont.h>

#include "nsIEventListener.h"
#include "nsIMenuListener.h"
#include "nsIMouseListener.h"
#include "nsGfxCIID.h"

#include "xlibrgb.h"

PRLogModuleInfo *XlibWidgetsLM = PR_NewLogModule("XlibWidgets");
PRLogModuleInfo *XlibScrollingLM = PR_NewLogModule("XlibScrolling");

// set up our static members here.
nsHashtable *nsWidget::gsWindowList = nsnull;

// this is a class for generating keys for
// the list of windows managed by mozilla.

// this is possibly the class impl that will be
// called whenever a new window is created/destroyed

//nsXlibWindowCallback *nsWidget::mWindowCallback = nsnull;

/* static */ nsXlibWindowCallback  nsWidget::gsWindowCreateCallback = nsnull;
/* static */ nsXlibWindowCallback  nsWidget::gsWindowDestroyCallback = nsnull;
/* static */ nsXlibEventDispatcher nsWidget::gsEventDispatcher = nsnull;

// this is for implemention the WM_PROTOCOL code
PRBool nsWidget::WMProtocolsInitialized = PR_FALSE;
Atom   nsWidget::WMDeleteWindow = 0;
Atom   nsWidget::WMTakeFocus = 0;
Atom   nsWidget::WMSaveYourself = 0;

// this is the window that has the focus
Window nsWidget::mFocusWindow = 0;

class nsWindowKey : public nsHashKey {
protected:
  Window mKey;

public:
  nsWindowKey(Window key) {
    mKey = key;
  }
  ~nsWindowKey(void) {
  }
  PRUint32 HashValue(void) const {
    return (PRUint32)mKey;
  }

  PRBool Equals(const nsHashKey *aKey) const {
    return (mKey == ((nsWindowKey *)aKey)->mKey);
  }

  nsHashKey *Clone(void) const {
    return new nsWindowKey(mKey);
  }
};

nsWidget::nsWidget() : nsBaseWidget()
{
  mPreferredWidth = 0;
  mPreferredHeight = 0;

  mDisplay = 0;
  mScreen = 0;
  mVisual = 0;
  mDepth = 0;

  mBaseWindow = 0;
  mBackground = NS_RGB(192, 192, 192);
  mBorderRGB = NS_RGB(192, 192, 192);
  mBackgroundPixel = xlib_rgb_xpixel_from_rgb(mBackground);
  mBackground = NS_RGB(192, 192, 192);
  mBorderPixel = xlib_rgb_xpixel_from_rgb(mBorderRGB);
  mGC = 0;
  mParentWidget = nsnull;
  mName = "unnamed";
  mIsShown = PR_FALSE;
  mIsToplevel = PR_FALSE;
  mVisibility = VisibilityFullyObscured; // this is an X constant.
  mWindowType = eWindowType_child;
  mBorderStyle = eBorderStyle_default;
}

nsWidget::~nsWidget()
{
  DestroyNative();
}

void
nsWidget::DestroyNative(void)
{
  if (mGC)
    XFreeGC(mDisplay, mGC);

  if (mBaseWindow) {
    XDestroyWindow(mDisplay, mBaseWindow);
    DeleteWindowCallback(mBaseWindow);
  }
}

NS_IMETHODIMP nsWidget::Create(nsIWidget *aParent,
                               const nsRect &aRect,
                               EVENT_CALLBACK aHandleEventFunction,
                               nsIDeviceContext *aContext,
                               nsIAppShell *aAppShell,
                               nsIToolkit *aToolkit,
                               nsWidgetInitData *aInitData)
{
  mParentWidget = aParent;

  return(StandardWidgetCreate(aParent, aRect, aHandleEventFunction,
                              aContext, aAppShell, aToolkit, aInitData,
                              nsnull));
}

NS_IMETHODIMP nsWidget::Create(nsNativeWidget aParent,
                               const nsRect &aRect,
                               EVENT_CALLBACK aHandleEventFunction,
                               nsIDeviceContext *aContext,
                               nsIAppShell *aAppShell,
                               nsIToolkit *aToolkit,
                               nsWidgetInitData *aInitData)
{
  return(StandardWidgetCreate(nsnull, aRect, aHandleEventFunction,
                              aContext, aAppShell, aToolkit, aInitData,
                              aParent));
}

static NS_DEFINE_IID(kWindowServiceCID,NS_XLIB_WINDOW_SERVICE_CID);
static NS_DEFINE_IID(kWindowServiceIID,NS_XLIB_WINDOW_SERVICE_IID);

nsresult
nsWidget::StandardWidgetCreate(nsIWidget *aParent,
                               const nsRect &aRect,
                               EVENT_CALLBACK aHandleEventFunction,
                               nsIDeviceContext *aContext,
                               nsIAppShell *aAppShell,
                               nsIToolkit *aToolkit,
                               nsWidgetInitData *aInitData,
                               nsNativeWidget aNativeParent)
{
  
  Window parent;

  mDisplay = xlib_rgb_get_display();
  mScreen = xlib_rgb_get_screen();
  mVisual = xlib_rgb_get_visual();
  mDepth = xlib_rgb_get_depth();

  // set up the BaseWidget parts.
  BaseCreate(aParent, aRect, aHandleEventFunction, aContext, 
             aAppShell, aToolkit, aInitData);

  // check to see if the parent is there...
  if (nsnull != aParent) {
    parent = ((aParent) ? (Window)aParent->GetNativeData(NS_NATIVE_WINDOW) : nsnull);
  } else {
    parent = (Window)aNativeParent;
  }
  // if there's no parent, make the parent the root window.
  if (parent == 0) {
    parent = RootWindowOfScreen(mScreen);
    mIsToplevel = PR_TRUE;
  }
  // set the bounds
  mBounds = aRect;
  mRequestedSize = aRect;


#ifndef MOZ_MONOLITHIC_TOOLKIT
  nsIXlibWindowService * xlibWindowService = nsnull;

   nsresult rv = nsServiceManager::GetService(kWindowServiceCID,
                                             kWindowServiceIID,
                                             (nsISupports **)&xlibWindowService);

  NS_ASSERTION(NS_SUCCEEDED(rv),"Couldn't obtain window service.");

  if (NS_OK == rv && nsnull != xlibWindowService)
  {
    xlibWindowService->GetWindowCreateCallback(&gsWindowCreateCallback);
    xlibWindowService->GetWindowDestroyCallback(&gsWindowDestroyCallback);
    
//     NS_ASSERTION(nsnull != gsWindowCreateCallback,"Window create callback is null.");
//     NS_ASSERTION(nsnull != gsWindowDestroyCallback,"Window destroy callback is null.");
    
    xlibWindowService->SetEventDispatcher((nsXlibEventDispatcher) nsAppShell::DispatchXEvent);

    NS_RELEASE(xlibWindowService);
  }
#endif /* !MOZ_MONOLITHIC_TOOLKIT */

  // call the native create function
  CreateNative(parent, mBounds);
  // set up our wm hints if it's appropriate
  if (mIsToplevel == PR_TRUE) {
    SetUpWMHints();
  }
  //  XSync(mDisplay, False);
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Destroy()
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Move(PRInt32 aX, PRInt32 aY)
{

  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::Move(x, y)\n"));
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("Moving window 0x%lx to %d, %d\n", mBaseWindow, aX, aY));
  mRequestedSize.x = aX;
  mRequestedSize.y = aY;
  if (mParentWidget) {
    ((nsWidget*)mParentWidget)->WidgetMove(this);
  } else {
    XMoveWindow(mDisplay, mBaseWindow, aX, aY);
  }
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Resize(PRInt32 aWidth,
                               PRInt32 aHeight,
                               PRBool   aRepaint)
{
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::Resize(width, height)\n"));

  if (aWidth <= 0) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("*** width is %d, fixing.\n", aWidth));
    aWidth = 1;
  }
  if (aHeight <= 0) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("*** height is %d, fixing.\n", aHeight));
    aHeight = 1;
  }
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("Resizing window 0x%lx to %d, %d\n", mBaseWindow, aWidth, aHeight));
  mRequestedSize.width = mBounds.width = aWidth;
  mRequestedSize.height = mBounds.height = aHeight;
  if (mParentWidget) {
    ((nsWidget *)mParentWidget)->WidgetResize(this);
  } else {
    XResizeWindow(mDisplay, mBaseWindow, aWidth, aHeight);
  }
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Resize(PRInt32 aX,
                               PRInt32 aY,
                               PRInt32 aWidth,
                               PRInt32 aHeight,
                               PRBool   aRepaint)
{
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::Resize(x, y, width, height)\n"));

  if (aWidth <= 0) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("*** width is %d, fixing.\n", aWidth));
    aWidth = 1;
  }
  if (aHeight <= 0) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("*** height is %d, fixing.\n", aHeight));
    aHeight = 1;
  }
  if (aX < 0) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("*** x is %d, fixing.\n", aX));
    aX = 0;
  }
  if (aY < 0) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("*** y is %d, fixing.\n", aY));
    aY = 0;
  }
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG,
         ("Resizing window 0x%lx to %d, %d\n", mBaseWindow, aWidth, aHeight));
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, 
         ("Moving window 0x%lx to %d, %d\n", mBaseWindow, aX, aY));
  mRequestedSize.x = aX;
  mRequestedSize.y = aY;
  mRequestedSize.width = mBounds.width = aWidth;
  mRequestedSize.height = mBounds.height = aHeight;
  if (mParentWidget) {
    ((nsWidget *)mParentWidget)->WidgetMoveResize(this);
  } else {
    XMoveResizeWindow(mDisplay, mBaseWindow, aX, aY, aWidth, aHeight);
  }
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Enable(PRBool bState)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::SetFocus(void)
{
  if (mBaseWindow) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::SetFocus() setting focus to 0x%lx\n", mBaseWindow));
    mFocusWindow = mBaseWindow;
  }
  return NS_OK;
}

Window
nsWidget::GetFocusWindow(void)
{
  return mFocusWindow;
}

NS_IMETHODIMP nsWidget::Invalidate(PRBool aIsSynchronous)
{
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::Invalidate(sync)\n"));
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Invalidate(const nsRect & aRect, PRBool aIsSynchronous)
{
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::Invalidate(rect, sync)\n"));
  return NS_OK;
}

nsIFontMetrics* nsWidget::GetFont(void)
{
  return nsnull;
}

NS_IMETHODIMP nsWidget::GetPreferredSize(PRInt32& aWidth, PRInt32& aHeight)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::SetMenuBar(nsIMenuBar * aMenuBar)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::ShowMenuBar(PRBool aShow)
{
  return NS_OK;
}

void * nsWidget::GetNativeData(PRUint32 aDataType)
{
  switch (aDataType) {
  case NS_NATIVE_WIDGET:
  case NS_NATIVE_WINDOW:
    return (void *)mBaseWindow;
    break;
  case NS_NATIVE_DISPLAY:
    return (void *)mDisplay;
    break;
  case NS_NATIVE_GRAPHIC:
    // XXX implement this...
    return (void *)mGC;
    break;
  default:
    fprintf(stderr, "nsWidget::GetNativeData(%d) called with crap value.\n",
            aDataType);
    return NULL;
    break;
  }
}

NS_IMETHODIMP nsWidget::SetTooltips(PRUint32 aNumberOfTips,
                                    nsRect* aTooltipAreas[])
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::UpdateTooltips(nsRect* aNewTips[])
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::RemoveTooltips()
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::SetFont(const nsFont &aFont)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::BeginResizingChildren(void)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::EndResizingChildren(void)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::SetColorMap(nsColorMap *aColorMap)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Show(PRBool bState)
{
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::Show()\n"));
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("state is %d\n", bState));

  if (bState) {
    if (mIsToplevel) {
      PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("Someone just used the show method on the toplevel window.\n"));
    }
    if (mParentWidget) {
      ((nsWidget *)mParentWidget)->WidgetShow(this);
    }
    else {
      if (mBaseWindow) {
        PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("Mapping window 0x%lx...\n", mBaseWindow));
        Map();
      }
    }
    mIsShown = PR_TRUE;
  }
  else {
    if (mBaseWindow) {
      PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("Unmapping window 0x%lx...\n", mBaseWindow));
      Unmap();
    }
    mIsShown = PR_FALSE;
  }
  return NS_OK;
}

NS_IMETHODIMP nsWidget::IsVisible(PRBool &aState)
{
  if (mVisibility != VisibilityFullyObscured) {
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::IsVisible: yes\n"));
    aState = PR_TRUE;
  }
  else {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::IsVisible: no\n"));
    aState = PR_FALSE;
  }
  return NS_OK;
}

NS_IMETHODIMP nsWidget::SetPreferredSize(PRInt32 aWidth, PRInt32 aHeight)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Update()
{
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::Update()\n"));
  return NS_OK;
}

NS_IMETHODIMP nsWidget::SetBackgroundColor(const nscolor &aColor)
{
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("nsWidget::SetBackgroundColor()\n"));

  nsBaseWidget::SetBackgroundColor(aColor);
  mBackgroundPixel = xlib_rgb_xpixel_from_rgb(mBackground);
  // set the window attrib
  XSetWindowBackground(mDisplay, mBaseWindow, mBackgroundPixel);
  return NS_OK;
}

NS_IMETHODIMP nsWidget::Scroll(PRInt32 aDx, PRInt32 aDy, nsRect *aClipRect)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::WidgetToScreen(const nsRect& aOldRect,
                                       nsRect& aNewRect)
{
  int x = 0;
  int y = 0;
  Window child;
  if (XTranslateCoordinates(mDisplay,
                            mBaseWindow,
                            RootWindowOfScreen(mScreen),
                            0, 0,
                            &x, &y,
                            &child) == True) {
    aNewRect.x = x + aOldRect.x;
    aNewRect.y = y + aOldRect.y;
  }
  return NS_OK;
}

NS_IMETHODIMP nsWidget::ScreenToWidget(const nsRect& aOldRect,
                                       nsRect& aNewRect)
{
  return NS_OK;
}

NS_IMETHODIMP nsWidget::SetCursor(nsCursor aCursor)
{
  if (!mBaseWindow)
    return NS_ERROR_FAILURE;

  // Only change cursor if it's changing
  if (aCursor != mCursor) {
    Cursor newCursor = 0;

    switch(aCursor) {
      case eCursor_select:
        newCursor = XCreateFontCursor(mDisplay, XC_xterm);
        break;

      case eCursor_wait:
        newCursor = XCreateFontCursor(mDisplay, XC_watch);
        break;

      case eCursor_hyperlink:
        newCursor = XCreateFontCursor(mDisplay, XC_hand2);
        break;

      case eCursor_standard:
        newCursor = XCreateFontCursor(mDisplay, XC_left_ptr);
        break;

      case eCursor_sizeWE:
      case eCursor_sizeNS:
        newCursor = XCreateFontCursor(mDisplay, XC_tcross);
        break;

      case eCursor_arrow_south:
      case eCursor_arrow_south_plus:
        newCursor = XCreateFontCursor(mDisplay, XC_bottom_side);
        break;

      case eCursor_arrow_north:
      case eCursor_arrow_north_plus:
        newCursor = XCreateFontCursor(mDisplay, XC_top_side);
        break;

      case eCursor_arrow_east:
      case eCursor_arrow_east_plus:
        newCursor = XCreateFontCursor(mDisplay, XC_right_side);
        break;

      case eCursor_arrow_west:
      case eCursor_arrow_west_plus:
        newCursor = XCreateFontCursor(mDisplay, XC_left_side);
        break;

      default:
        NS_ASSERTION(PR_FALSE, "Invalid cursor type");
        break;
    }

    if (nsnull != newCursor) {
      mCursor = aCursor;
      XDefineCursor(mDisplay, mBaseWindow, newCursor);
    }
  }
  return NS_OK;
}

NS_IMETHODIMP nsWidget::PreCreateWidget(nsWidgetInitData *aInitData)
{
  if (nsnull != aInitData) {
    SetWindowType(aInitData->mWindowType);
    SetBorderStyle(aInitData->mBorderStyle);
    
    return NS_OK;
  }
  return NS_ERROR_FAILURE;
}

nsIWidget *nsWidget::GetParent(void)
{
  if (nsnull != mParentWidget) {
    NS_ADDREF(mParentWidget);
  }
  return mParentWidget;
}

void nsWidget::CreateNative(Window aParent, nsRect aRect)
{
  XSetWindowAttributes attr;
  unsigned long attr_mask;

  // on a window resize, we don't want to window contents to
  // be discarded...
  attr.bit_gravity = NorthWestGravity;
  // make sure that we listen for events
  attr.event_mask = GetEventMask();

  // set the default background color and border to that awful gray
  attr.background_pixel = mBackgroundPixel;
  attr.border_pixel = mBorderPixel;
  // set the colormap
  attr.colormap = xlib_rgb_get_cmap();
  // here's what's in the struct
  attr_mask = CWBitGravity | CWEventMask | CWBackPixel | CWBorderPixel;
  // check to see if there was actually a colormap.
  if (attr.colormap)
    attr_mask |= CWColormap;

  CreateNativeWindow(aParent, mBounds, attr, attr_mask);
  CreateGC();
}

/* virtual */ long
nsWidget::GetEventMask()
{
	long event_mask;

	event_mask = 
		ExposureMask | 
		ButtonPressMask | 
		ButtonReleaseMask | 
		PointerMotionMask |
    VisibilityChangeMask |
    KeyPressMask |
    KeyReleaseMask |
    StructureNotifyMask;

	return event_mask;
}

void nsWidget::CreateNativeWindow(Window aParent, nsRect aRect,
                                  XSetWindowAttributes aAttr, unsigned long aMask)
{
  int width;
  int height;

  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, 
         ("*** Warning: nsWidget::CreateNative falling back to sane default for widget type \"%s\"\n", 
          (const char *) nsAutoCString(mName)));

  if (mName == "unnamed") {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG,
           ("What freaking widget is this, anyway?\n"));
  }
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("Creating XWindow: x %d y %d w %d h %d\n",
                                       aRect.x, aRect.y, aRect.width, aRect.height));

  if (aRect.width <= 0) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("*** Fixing width...\n"));
    width = 1;
  }
  else {
    width = aRect.width;
  }
  if (aRect.height <= 0) {
    PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("*** Fixing height...\n"));
    height = 1;
  }
  else {
    height = aRect.height;
  }
  
  mBaseWindow = XCreateWindow(mDisplay,
                              aParent,
                              aRect.x, aRect.y,
                              width, height,
                              0,                // border width
                              mDepth,           // depth
                              InputOutput,      // class
                              mVisual,          // visual
                              aMask,
                              &aAttr);
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, 
         ("nsWidget: Created window 0x%lx with parent 0x%lx %s\n",
          mBaseWindow, aParent, (mIsToplevel ? "TopLevel" : "")));
  // XXX when we stop getting lame values for this remove it.
  // sometimes the dimensions have been corrected by the code above.
  mRequestedSize.height = mBounds.height = height;
  mRequestedSize.width = mBounds.width = width;
  // add the callback for this
  AddWindowCallback(mBaseWindow, this);
}

nsWidget *
nsWidget::GetWidgetForWindow(Window aWindow)
{
  if (gsWindowList == nsnull) {
    return nsnull;
  }
  nsWindowKey *window_key = new nsWindowKey(aWindow);
  nsWidget *retval = (nsWidget *)gsWindowList->Get(window_key);
  delete window_key;
  return retval;
}

void
nsWidget::AddWindowCallback(Window aWindow, nsWidget *aWidget)
{
  // make sure that the list has been initialized
  if (gsWindowList == nsnull) {
    gsWindowList = new nsHashtable();
  }
  nsWindowKey *window_key = new nsWindowKey(aWindow);
  gsWindowList->Put(window_key, aWidget);
  // add a new ref to this widget
  NS_ADDREF(aWidget);

  // make sure that if someone is listening that we inform
  // them of the new window
  if (gsWindowCreateCallback) 
  {
    (*gsWindowCreateCallback)(aWindow);
  }

  delete window_key;
}

void
nsWidget::DeleteWindowCallback(Window aWindow)
{
  nsWindowKey *window_key = new nsWindowKey(aWindow);
  nsWidget *widget = (nsWidget *)gsWindowList->Get(window_key);
  NS_RELEASE(widget);
  gsWindowList->Remove(window_key);

  if (gsWindowDestroyCallback) 
  {
    (*gsWindowDestroyCallback)(aWindow);
  }

  delete window_key;
}

#undef TRACE_PAINT
#undef TRACE_PAINT_FLASH

#ifdef TRACE_PAINT_FLASH
#include "nsXUtils.h" // for nsXUtils::XFlashWindow()
#endif

PRBool
nsWidget::OnPaint(nsPaintEvent &event)
{
  nsresult result = PR_FALSE;
  if (mEventCallback) {
    event.renderingContext = nsnull;
    static NS_DEFINE_IID(kRenderingContextCID, NS_RENDERING_CONTEXT_CID);
    static NS_DEFINE_IID(kRenderingContextIID, NS_IRENDERING_CONTEXT_IID);
    if (NS_OK == nsComponentManager::CreateInstance(kRenderingContextCID,
                                                    nsnull,
                                                    kRenderingContextIID,
                                                    (void **)&event.renderingContext)) {
      event.renderingContext->Init(mContext, this);
      result = DispatchWindowEvent(event);
      NS_RELEASE(event.renderingContext);
    }
    else {
      result = PR_FALSE;
    }

#ifdef TRACE_PAINT
	static PRInt32 sPrintCount = 0;

    if (event.rect) 
	{
      printf("%4d nsWidget::OnPaint   (this=%p,name=%s,xid=%p,rect=%d,%d,%d,%d)\n", 
			 sPrintCount++,
			 (void *) this,
			 (const char *) nsCAutoString(mName),
			 (void *) mBaseWindow,
			 event.rect->x, 
			 event.rect->y,
			 event.rect->width, 
			 event.rect->height);
    }
    else 
	{
      printf("%4d nsWidget::OnPaint   (this=%p,name=%s,xid=%p,rect=none)\n", 
			 sPrintCount++,
			 (void *) this,
			 (const char *) nsCAutoString(mName),
			 (void *) mBaseWindow);
    }
#endif

#ifdef TRACE_PAINT_FLASH
    XRectangle ar;
    XRectangle * area = NULL;

    if (event.rect)
    {
      ar.x = event.rect->x;
      ar.y = event.rect->y;

      ar.width = event.rect->width;
      ar.height = event.rect->height;

      area = &ar;
    }

    nsXUtils::XFlashWindow(mDisplay,mBaseWindow,1,100000,area);
#endif

  }
  return result;
}

PRBool nsWidget::OnDeleteWindow(void)
{
  printf("nsWidget::OnDeleteWindow()\n");
  nsBaseWidget::OnDestroy();
  // emit a destroy signal
  return DispatchDestroyEvent();
}

PRBool nsWidget::DispatchDestroyEvent(void) {
  PRBool result = PR_FALSE;
  if (nsnull != mEventCallback) {
    nsGUIEvent event;
    event.eventStructType = NS_GUI_EVENT;
    event.message = NS_DESTROY;
    event.widget = this;
    event.time = 0;
    event.point.x = 0;
    event.point.y = 0;
    AddRef();
    result = DispatchWindowEvent(event);
    Release();
  }
  return result;
}

PRBool nsWidget::DispatchMouseEvent(nsMouseEvent& aEvent) 
{
  PRBool result = PR_FALSE;
  if (nsnull == mEventCallback && nsnull == mMouseListener) {
    return result;
  }

  if (nsnull != mEventCallback) {
    result = DispatchWindowEvent(aEvent);
    return result;
  }
  if (nsnull != mMouseListener) {
    switch (aEvent.message) {
    case NS_MOUSE_MOVE:
      // XXX this isn't handled in gtk for some reason.
      break;
    case NS_MOUSE_LEFT_BUTTON_DOWN:
    case NS_MOUSE_MIDDLE_BUTTON_DOWN:
    case NS_MOUSE_RIGHT_BUTTON_DOWN:
      result = ConvertStatus(mMouseListener->MousePressed(aEvent));
      break;
    case NS_MOUSE_LEFT_BUTTON_UP:
    case NS_MOUSE_MIDDLE_BUTTON_UP:
    case NS_MOUSE_RIGHT_BUTTON_UP:
      result = ConvertStatus(mMouseListener->MouseReleased(aEvent));
      result = ConvertStatus(mMouseListener->MouseClicked(aEvent));
      break;
    }
  }
  return result;
}

PRBool
nsWidget::OnResize(nsSizeEvent &event)
{
  nsresult result = PR_FALSE;
  if (mEventCallback) {
      result = DispatchWindowEvent(event);
  }
  return result;
}

PRBool nsWidget::DispatchWindowEvent(nsGUIEvent & aEvent)
{
  nsEventStatus status;
  DispatchEvent(&aEvent, status);
  return ConvertStatus(status);
}

PRBool nsWidget::DispatchKeyEvent(nsKeyEvent & aKeyEvent)
{
  if (mEventCallback) 
  {
    return DispatchWindowEvent(aKeyEvent);
  }

  return PR_FALSE;
}

//////////////////////////////////////////////////////////////////
//
// Turning TRACE_EVENTS on will cause printfs for all
// mouse events that are dispatched.
//
// These are extra noisy, and thus have their own switch:
//
// NS_MOUSE_MOVE
// NS_PAINT
// NS_MOUSE_ENTER, NS_MOUSE_EXIT
//
//////////////////////////////////////////////////////////////////

#undef TRACE_EVENTS
#undef TRACE_EVENTS_MOTION
#undef TRACE_EVENTS_PAINT
#undef TRACE_EVENTS_CROSSING

#ifdef DEBUG
void
nsWidget::DebugPrintEvent(nsGUIEvent &   aEvent,
                          Window         aWindow)
{
#ifndef TRACE_EVENTS_MOTION
  if (aEvent.message == NS_MOUSE_MOVE)
  {
    return;
  }
#endif

#ifndef TRACE_EVENTS_PAINT
  if (aEvent.message == NS_PAINT)
  {
    return;
  }
#endif

#ifndef TRACE_EVENTS_CROSSING
  if (aEvent.message == NS_MOUSE_ENTER || aEvent.message == NS_MOUSE_EXIT)
  {
    return;
  }
#endif

  static int sPrintCount=0;

  printf("%4d %-26s(this=%-8p , window=%-8p",
         sPrintCount++,
         (const char *) nsAutoCString(debug_GuiEventToString(&aEvent)),
         this,
         (void *) aWindow);
         
  printf(" , x=%-3d, y=%d)",aEvent.point.x,aEvent.point.y);

  printf("\n");
}
#endif // DEBUG
//////////////////////////////////////////////////////////////////


NS_IMETHODIMP nsWidget::DispatchEvent(nsGUIEvent * aEvent,
                                      nsEventStatus &aStatus)
{
#ifdef TRACE_EVENTS
  DebugPrintEvent(*aEvent,mBaseWindow);
#endif

  NS_ADDREF(aEvent->widget);

  if (nsnull != mMenuListener) {
    if (NS_MENU_EVENT == aEvent->eventStructType)
      aStatus = mMenuListener->MenuSelected(NS_STATIC_CAST(nsMenuEvent&, *aEvent));
  }

  aStatus = nsEventStatus_eIgnore;
  if (nsnull != mEventCallback) {
    aStatus = (*mEventCallback)(aEvent);
  }

  // Dispatch to event listener if event was not consumed
  if ((aStatus != nsEventStatus_eIgnore) && (nsnull != mEventListener)) {
    aStatus = mEventListener->ProcessEvent(*aEvent);
  }

  NS_RELEASE(aEvent->widget);

  return NS_OK;
}

PRBool nsWidget::ConvertStatus(nsEventStatus aStatus)
{
  switch(aStatus) {
    case nsEventStatus_eIgnore:
      return(PR_FALSE);
    case nsEventStatus_eConsumeNoDefault:
      return(PR_TRUE);
    case nsEventStatus_eConsumeDoDefault:
      return(PR_FALSE);
    default:
      NS_ASSERTION(0, "Illegal nsEventStatus enumeration value");
      break;
  }
  return(PR_FALSE);
}

void nsWidget::CreateGC(void)
{
  mGC = XCreateGC(mDisplay, mBaseWindow, 0, NULL);
}

void nsWidget::WidgetPut(nsWidget *aWidget)
{
  
}

void nsWidget::WidgetMove(nsWidget *aWidget)
{
  PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("nsWidget::WidgetMove()\n"));
  if (PR_TRUE == WidgetVisible(aWidget->mRequestedSize)) {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Widget is visible...\n"));
    XMoveWindow(aWidget->mDisplay, aWidget->mBaseWindow,
                aWidget->mRequestedSize.x, aWidget->mRequestedSize.y);
    if (aWidget->mIsShown == PR_TRUE) {
      PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Mapping window 0x%lx...\n", aWidget->mBaseWindow));
      aWidget->Map();
    }
  }
  else {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Widget is not visible...\n"));
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Unmapping window 0x%lx...\n", aWidget->mBaseWindow));
    aWidget->Unmap();
  }
}

void nsWidget::WidgetResize(nsWidget *aWidget)
{
  PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("nsWidget::WidgetResize()\n"));
  // note that we do the resize before a map.
  if (PR_TRUE == WidgetVisible(aWidget->mRequestedSize)) {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Widget is visible...\n"));
    XResizeWindow(aWidget->mDisplay, aWidget->mBaseWindow,
                  aWidget->mRequestedSize.width,
                  aWidget->mRequestedSize.height);
    if (aWidget->mIsShown == PR_TRUE) {
      PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Mapping window 0x%lx...\n", aWidget->mBaseWindow));
      aWidget->Map();
    }
  }
  else {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Widget is not visible...\n"));
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Unmapping window 0x%lx...\n", aWidget->mBaseWindow));
    aWidget->Unmap();
  }
}

void nsWidget::WidgetMoveResize(nsWidget *aWidget)
{
  PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("nsWidget::WidgetMoveResize()\n"));
  if (PR_TRUE == WidgetVisible(aWidget->mRequestedSize)) {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Widget is visible...\n"));
    XResizeWindow(aWidget->mDisplay,
                  aWidget->mBaseWindow,
                  aWidget->mRequestedSize.width, aWidget->mRequestedSize.height);
    XMoveWindow(aWidget->mDisplay, aWidget->mBaseWindow,
                aWidget->mRequestedSize.x, aWidget->mRequestedSize.y);
    if (aWidget->mIsShown == PR_TRUE) {
      PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Mapping window 0x%lx...\n", aWidget->mBaseWindow));
      aWidget->Map();
    }
  }
  else {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Widget is not visible...\n"));
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Unmapping window 0x%lx...\n", aWidget->mBaseWindow));
    aWidget->Unmap();
  }
}

void nsWidget::WidgetShow(nsWidget *aWidget)
{
  if (PR_TRUE == WidgetVisible(aWidget->mRequestedSize)) {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Mapping window 0x%lx...\n", aWidget->mBaseWindow));
    aWidget->Map();
  }
  else {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("Not Mapping window...\n"));
  }
}

PRBool nsWidget::WidgetVisible(nsRect &aBounds)
{
  nsRect scrollArea;
  scrollArea.x = 0;
  scrollArea.y = 0;
  scrollArea.width = mRequestedSize.width + 1;
  scrollArea.height = mRequestedSize.height + 1;
  if (scrollArea.Intersects(aBounds)) {
    PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("nsWidget::WidgetVisible(): widget is visible\n"));
    return PR_TRUE;
  }
  PR_LOG(XlibScrollingLM, PR_LOG_DEBUG, ("nsWidget::WidgetVisible(): widget is not visible\n"));
  return PR_FALSE;
}

void nsWidget::Map(void)
{
  XMapWindow(mDisplay, mBaseWindow);
}

void nsWidget::Unmap(void)
{
  XUnmapWindow(mDisplay, mBaseWindow);
}

void nsWidget::SetVisibility(int aState)
{
  mVisibility = aState;
}

// nsresult
// nsWidget::SetXlibWindowCallback(nsXlibWindowCallback *aCallback)
// {
//   if (aCallback == nsnull) {
//     return NS_ERROR_FAILURE;
//   }
//   else {
//     mWindowCallback = aCallback;
//   }
//   return NS_OK;
// }

// nsresult
// nsWidget::XWindowCreated(Window aWindow) {
//   if (mWindowCallback) {
//     mWindowCallback->WindowCreated(aWindow);
//   }
//   return NS_OK;
// }

// nsresult
// nsWidget::XWindowDestroyed(Window aWindow) {
//   if (mWindowCallback) {
//     mWindowCallback->WindowDestroyed(aWindow);
//   }
//   return NS_OK;
// }

void
nsWidget::SetUpWMHints(void) {
  // check to see if we need to get the atoms for the protocols
  if (WMProtocolsInitialized == PR_FALSE) {
    WMDeleteWindow = XInternAtom(mDisplay, "WM_DELETE_WINDOW", True);
    WMTakeFocus = XInternAtom(mDisplay, "WM_TAKE_FOCUS", True);
    WMSaveYourself = XInternAtom(mDisplay, "WM_SAVE_YOURSELF", True);
    WMProtocolsInitialized = PR_TRUE;
  }
  Atom WMProtocols[2];
  WMProtocols[0] = WMDeleteWindow;
  WMProtocols[1] = WMTakeFocus;
  // note that we only set two of the above protocols
  PR_LOG(XlibWidgetsLM, PR_LOG_DEBUG, ("Setting up wm hints for window 0x%lx\n",
                                       mBaseWindow));
  XSetWMProtocols(mDisplay, mBaseWindow, WMProtocols, 2);
}

NS_METHOD nsWidget::SetBounds(const nsRect &aRect)
{
  mRequestedSize = aRect;
  return nsBaseWidget::SetBounds(aRect);
}

NS_METHOD nsWidget::GetRequestedBounds(nsRect &aRect)
{
  aRect = mRequestedSize;
  return NS_OK;
}
