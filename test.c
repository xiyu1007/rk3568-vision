#include <opencv2/opencv.hpp>

int main()
{
    cv::Mat img = cv::Mat::zeros(100, 100, CV_8UC3);
    cv::imshow("test", img);
    cv::waitKey(0);
}