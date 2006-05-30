// -*- mode:C++; tab-width:4; c-basic-offset:4; indent-tabs-mode:nil -*-
#ifndef __YARP2_FRAME_GRABBER__
#define __YARP2_FRAME_GRABBER__

#include <yarp/dev/DeviceDriver.h>

#include <yarp/sig/Image.h>

/*! \file FrameGrabber.h define common interfaces for framer grabber devices */

namespace yarp{
    namespace dev {
        class FrameGrabber;
        class FrameGrabberOpenParameters;
    }
}

/** 
 * Common interface to a FrameGrabber.
 */
class yarp::dev::FrameGrabber: public DeviceDriver
{
public:
    virtual ~FrameGrabber(){}
    /**
     * Get a raw buffer from the frame grabber
     * @param buffer: pointer to the buffer to be filled (must be previously allocated)
     * @return true/false upon success/failure
     */
    virtual bool getBuffer(unsigned char *buffer)=0;


    virtual bool getImage(yarp::sig::ImageOf<yarp::sig::PixelRgb>& image) = 0;

    /** 
     * Return the height of each frame.
     * @return image height
     */
    virtual int getHeight()=0;

    /** 
     * Return the width of each frame.
     * @return image width
     */
    virtual int getWidth()=0;
  
};

#endif
