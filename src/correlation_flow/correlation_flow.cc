// Copyright (c) <2017>, <Nanyang Technological University> All rights reserved.

// This file is part of correlation_flow.

//     correlation_flow is free software: you can redistribute it and/or modify
//     it under the terms of the GNU General Public License as published by
//     the Free Software Foundation, either version 3 of the License, or
//     (at your option) any later version.

//     Foobar is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU General Public License for more details.

//     You should have received a copy of the GNU General Public License
//     along with Foobar.  If not, see <http://www.gnu.org/licenses/>.

#include <math.h>
#include <ros/ros.h>
#include "correlation_flow/correlation_flow.h"


CorrelationFlow::CorrelationFlow(ros::NodeHandle nh):nh(nh)
{
    if(!nh.getParam("image_width", width)) ROS_ERROR("Can't get Param image_width");
    if(!nh.getParam("image_height", height)) ROS_ERROR("Can't get Param image_height");
    if(!nh.getParam("focal_x", focal_x)) ROS_ERROR("Can't get Param focal_x");
    if(!nh.getParam("focal_y", focal_y)) ROS_ERROR("Can't get Param focal_y");

    velocity = Vector3d::Zero();
    lowpass_weight = 0.10;

    lamda = 0.1;
    sigma = 0.2;

    ArrayXXf target = ArrayXXf::Zero(width, height);
    target(width/2, height/2) = 1;
    target_fft = fft(target);
    filter_fft = fft(ArrayXXf::Zero(width, height));

    initialized = false;

    pub_twist = nh.advertise<geometry_msgs::TwistStamped>("vision_speed/speed_twist", 1);
    pub_vector = nh.advertise<geometry_msgs::Vector3Stamped>("vision_speed/speed_vector", 1);
}


void CorrelationFlow::callback(const sensor_msgs::ImageConstPtr& msg)
{
    timer.tic();
    image = cv_bridge::toCvShare(msg, "mono8")->image;
    image(cv::Rect((image.cols-width)/2, (image.rows-height)/2, width, height)).convertTo(sample_cv, CV_32FC1, 1/255.0);
    
    sample = Eigen::Map<ArrayXXf>(&sample_cv.at<float>(0,0), width, height);

    // ArrayXXf lp= log_polar(sample_cv);
    sample_fft = fft(sample);

    if (initialized == false)
    {   
        train_fft = sample_fft;
        kernel = gaussian_kernel();
        filter_fft = target_fft/(kernel + lamda);
        initialized = true;
        ros_time = msg->header.stamp.toSec();
        ROS_WARN("initialized.");
        return;
    }

    // motion of current frame
    kernel = gaussian_kernel(sample_fft);
    output = ifft(filter_fft*kernel);
    max_response = output.maxCoeff(&(max_index[0]), &(max_index[1]));
    float trans_psr = get_psr(output, max_index[0], max_index[1]);
    
    // update filter
    train_fft = sample_fft;
    kernel = gaussian_kernel();
    filter_fft = target_fft/(kernel + lamda);

    // update ROS TIME
    double dt = msg->header.stamp.toSec() - ros_time;
    ros_time = msg->header.stamp.toSec();
    if(dt<1e-5) {ROS_WARN("image msg time stamp is INVALID, set dt=0.03s"); dt=0.03;}


    // veclocity calculation
    Vector3d v = Vector3d(-1.0*((max_index[0]-width/2)/dt)/focal_x, -1.0*((max_index[1]-height/2)/dt)/focal_y, 0);
    velocity = lowpass_weight * v + (1-lowpass_weight) * velocity; // low pass filter
    // rotation = (max_indexR[0]-target_dim/2)*rot_resolution;
    // wz = (rotation*M_PI/180.0)/dt;

    publish(msg->header);
    timer.toc("callback:");
    ROS_WARN("vx=%f, vy=%f, vz=%f m/s with psr: %f", velocity(0), velocity(1), velocity(2), trans_psr);
}


inline void CorrelationFlow::publish(const std_msgs::Header header)
{
    geometry_msgs::TwistStamped twist;
    twist.header.stamp = header.stamp;
    tf::vectorEigenToMsg(velocity, twist.twist.linear);
    pub_twist.publish(twist);

    geometry_msgs::Vector3Stamped vector;
    vector.header.stamp = header.stamp;
    tf::vectorEigenToMsg(velocity, vector.vector);
    pub_vector.publish(vector);
}



inline ArrayXXcf CorrelationFlow::fft(const ArrayXXf& x)
{
    ArrayXXcf xf = ArrayXXcf(x.rows()/2+1, x.cols());

    fft_plan = fftwf_plan_dft_r2c_2d(x.cols(), x.rows(), (float(*))(x.data()), 
        (float(*)[2])(xf.data()), FFTW_ESTIMATE); // reverse order for column major
    
    fftwf_execute(fft_plan);
    
    return xf;
}


inline ArrayXXf CorrelationFlow::ifft(const ArrayXXcf& xf)
{
    ArrayXXf x = ArrayXXf((xf.rows()-1)*2, xf.cols());
    
    ArrayXXcf cxf = xf;
    
    fft_plan = fftwf_plan_dft_c2r_2d(xf.cols(), (xf.rows()-1)*2, (float(*)[2])(cxf.data()),
        (float(*))(x.data()), FFTW_ESTIMATE);
    
    fftwf_execute(fft_plan);
    
    return x/x.size();
}


inline ArrayXXcf CorrelationFlow::gaussian_kernel()
{
    unsigned int N = height * width;

    train_square = train_fft.square().abs().sum()/N; // Parseval's Theorem

    float xx = train_square;

    float yy = train_square;

    train_fft_conj = train_fft.conjugate();
    
    xyf = train_fft * train_fft_conj;
    
    xy = ifft(xyf);

    xxyy = (xx+yy-2*xy)/N;

    return fft((-1/(sigma*sigma)*xxyy).exp());
}


inline ArrayXXcf CorrelationFlow::gaussian_kernel(const ArrayXXcf& xf)
{
    unsigned int N = height * width;

    float xx = xf.square().abs().sum()/N; // Parseval's Theorem

    float yy = train_square;
    
    xyf = xf * train_fft_conj;
    
    xy = ifft(xyf);

    xxyy = (xx+yy-2*xy)/N;

    return fft((-1/(sigma*sigma)*xxyy).exp());
}


inline ArrayXXf CorrelationFlow::log_polar(const cv::Mat img)
{
    cv::Mat log_polar_img;

    cv::Point2f center((float)img.cols/2, (float)img.rows/2);

    double radius = (double)img.rows / 2;

    double M = (double)img.cols / log(radius);

    cv::logPolar(img, log_polar_img, center, M, cv::INTER_LINEAR + cv::WARP_FILL_OUTLIERS);

    return Eigen::Map<ArrayXXf>(&log_polar_img.at<float>(0,0), img.cols, img.rows);
}


inline float CorrelationFlow::get_psr(const ArrayXXf& output, ArrayXXf::Index x, ArrayXXf::Index y)
{
    float max_output = output(x, y);

    float side_lobe_mean = (output.sum()-max_output)/(output.size()-1);

    float std  = sqrt((output-side_lobe_mean).square().mean());

    return (max_response - side_lobe_mean)/std;
}


// inline void CorrelationFlow::save_file(geometry_msgs::TwistStamped twist, string filename)
// {
//     file.open(filename.c_str(), ios::app);
//     file<<boost::format("%.9f") % (twist.header.stamp.toSec())<<" "
//         <<twist.twist.linear.x<<" "
//         <<twist.twist.linear.y<<" "
//         <<0<<" "
//         <<0<<" "
//         <<0<<" "
//         <<0<<" "
//         <<0<<endl;
//     file.close();
// }
