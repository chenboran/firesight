#include <string.h>
#include <math.h>
#include <set>
#include <iostream>
#include <stdexcept>
#include "FireLog.h"
#include "FireSight.hpp"
#include "opencv2/features2d/features2d.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/calib3d/calib3d.hpp"
#include "jansson.h"
#include "jo_util.hpp"
#include "MatUtil.hpp"
#include "version.h"

using namespace cv;
using namespace std;
using namespace firesight;

json_t * json_matrix(const Mat &mat) {
    json_t *jmat = json_array();
    for (int r=0; r < mat.rows; r++) {
        for (int c=0; c < mat.cols; c++) {
            json_array_append(jmat, json_real(mat.at<double>(r,c)));
        }
    }
    return jmat;
}

enum CompareOp {
    COMPARE_XY,
    COMPARE_YX
};

typedef class ComparePoint2f {
    private:
        CompareOp op;

    public:
        ComparePoint2f(CompareOp op=COMPARE_XY) {
            this->op = op;
        }
    public:
        bool operator()(const Point2f &lhs, const Point2f &rhs) const {
            assert(!isnan(lhs.x));
            assert(!isnan(rhs.x));
            int cmp;
            switch (op) {
            case COMPARE_XY:
                cmp = lhs.x - rhs.x;
                if (cmp == 0) {
                    cmp = lhs.y - rhs.y;
                }
                break;
            case COMPARE_YX:
                cmp = lhs.y - rhs.y;
                if (cmp == 0) {
                    cmp = lhs.x - rhs.x;
                }
                break;
            }
            return cmp < 0;
        }
} ComparePoint2f;

typedef struct GridMatcher {
    vector<Point2f> imagePts;
    vector<Point3f> objectPts;
    Point3f objTotals;
    Point2f imgTotals;
    Rect imgRect;
    const ComparePoint2f cmpYX;
    set<Point2f,ComparePoint2f> imgSet;
    Size imgSize;
    Point2f imgSep;
    Point2f objSep;
    Mat gridIndexes;	// object grid matrix of imagePts/objectPts vector indexes or -1
    GridMatcher(Size imgSize, Point2f imgSep, Point2f objSep)
        : cmpYX(ComparePoint2f(COMPARE_YX)), imgSet(set<Point2f,ComparePoint2f>(cmpYX)) {
        this->imgSize = imgSize;
        this->imgSep = imgSep;
        this->objSep = objSep;
        this->imgRect = Rect(imgSize.width/2, imgSize.height/2, 0, 0);
    }

    bool add(Point2f &ptImg, Point3f &ptObj) {
        set<Point2f,ComparePoint2f>::iterator it = imgSet.find(ptImg);

        if (it != imgSet.end()) {
            return false;
        }
        objectPts.push_back(ptObj);
        imagePts.push_back(ptImg);
        objTotals += ptObj;
        imgTotals += ptImg;
        imgSet.insert(ptImg);
        return true;
    }

    void calcGridIndexes() {
        int ny = imgSize.height/imgSep.y+1.5;
        int nx = imgSize.width/imgSep.x+1.5;
        gridIndexes = Mat(Size(nx,ny),CV_16S);
        gridIndexes = -1;
        cout << "ny:" << ny << " nx:" << nx << " gridIndexes:" << matInfo(gridIndexes) << endl;
        for (short i=0; i < objectPts.size(); i++) {
            int r = objectPts[i].y;
            int c = objectPts[i].x;
            cout << "SUBMAT [" << r << "," << c << "] = " << i << endl;
            assert( 0 <= r && r < gridIndexes.rows);
            assert( 0 <= c && c < gridIndexes.cols);
            gridIndexes.at<short>(r,c) = i;
        }
    }

    void addSubImage(int row, int col, int rows, int cols, int minPts,
                   vector<vector<Point2f> > &vImagePts, vector<vector<Point3f> > &vObjectPts) {
		vector<Point2f> subImgPts;
		vector<Point3f> subObjPts;
        cout << "addSubImage" << gridIndexes.rows << endl;
        float cy = (rows-1)/2.0;
        float cx = (cols-1)/2.0;
        float z = 0;
        for (int r=0; r < rows; r++) {
            for (int c=0; c < cols; c++) {
                short index = gridIndexes.at<short>(r+row, c+col);
                if (0 <= index) {
                    Point2f ptImg = imagePts[index];
                    Point3f ptObj = objectPts[index];
                    Point3f subObjPt(objSep.x*(ptObj.x-cx), objSep.y*(ptObj.y-cy), z);
                    cout << "index:" << index << " r:" << r << " c:" << c 
						<< " ptImg:" << ptImg << " subObjPt:" << subObjPt << endl;
                    subImgPts.push_back(ptImg);
                    subObjPts.push_back(subObjPt);
                }
            }
        }
        cout << "addSubImage return" << endl;
		if (subImgPts.size() >= minPts) {
			vObjectPts.push_back(subObjPts);
			vImagePts.push_back(subImgPts);
		}
    }

    int size() {
        return objectPts.size();
    }

    Point2f getImageCentroid() {
        int n = objectPts.size();
        return Point2f(imgTotals.x/n, imgTotals.y/n);
    }

    Point3f getObjectCentroid() {
        int n = objectPts.size();
        return Point3f(objTotals.x/n, objTotals.y/n, objTotals.z/n);
    }

    string calibrateImage(json_t *pStageModel, Mat &cameraMatrix, Mat &distCoeffs) {
		string errMsg;
        vector<Mat> rvecs;
        vector<Mat> tvecs;
        vector< vector<Point3f> > vObjectPts;
        vector< vector<Point2f> > vImagePts;

        calcGridIndexes();
        int subRows = 7;
        int subCols = 8;
		int minPts = max(4, subRows * subCols * 2 / 3);
		int dr = (gridIndexes.rows-subRows)/3;
		int dc = (gridIndexes.cols-subCols)/3;
        for (int r=0; r < gridIndexes.rows-subRows; r += dr) {
            for (int c=0; c < gridIndexes.cols-subCols; c += dc) {
                addSubImage(r, c, subRows, subCols, minPts, vImagePts, vObjectPts);
            }
        }

		double rmserror;
        cout << "calibrateCamera images:" << vImagePts.size() << endl;

		try {
			rmserror = calibrateCamera(vObjectPts, vImagePts, imgSize,
                                          cameraMatrix, distCoeffs, rvecs, tvecs);
		} catch (cv::Exception ex) {
			errMsg = "calibrateImage(FAILED) ";
			errMsg += ex.msg;
		} catch (const char * err) {
			errMsg = "calibrateImage(FAILED) ";
			errMsg += err;
		} catch (...) {
			errMsg = "calibrateImage(FAILED...)";
		}
        cout << "calibrateCamera => " << rmserror << endl;

        json_t *pCalibrate = json_object();
        json_object_set(pStageModel, "calibrate", pCalibrate);

        json_object_set(pCalibrate, "camera", json_matrix(cameraMatrix));
        json_object_set(pCalibrate, "distCoeffs", json_matrix(distCoeffs));
        json_object_set(pCalibrate, "rmserror", json_real(rmserror));
        json_object_set(pCalibrate, "images", json_real(vImagePts.size()));
#ifdef RVECS_TVECS
        json_t *pRvecs = json_array();
        json_object_set(pCalibrate, "rvecs", pRvecs);
        for (int i=0; i < rvecs.size(); i++) {
            json_array_append(pRvecs, json_matrix(rvecs[i]));
        }
        json_t *pTvecs = json_array();
        json_object_set(pCalibrate, "tvecs", pTvecs);
        for (int i=0; i < tvecs.size(); i++) {
            json_array_append(pTvecs, json_matrix(tvecs[i]));
        }
#endif
		return errMsg;
    }
} GridMatcher;

typedef map<Point2f,Point2f,ComparePoint2f> PointMap;

static string identifyRows(json_t *pStageModel, vector<Point2f> &pointsXY, float &dyMedian, Point2f &dyTot1,
                           Point2f &dyTot2, int &dyCount1, int &dyCount2, double tolerance, int sepY, float &gridY)
{
    vector<float> dyList;
    Point2f prevPt;
    for (vector<Point2f>::iterator it=pointsXY.begin(); it!=pointsXY.end(); it++) {
        if (it != pointsXY.begin()) {
            float dy = prevPt.y - it->y;
            dyList.push_back(dy);
        }
        prevPt = *it;
    }
    sort(dyList.begin(), dyList.end());
    dyMedian = dyList[dyList.size()/2];
    float maxTol = dyMedian < 0 ? 1-tolerance : 1+tolerance;
    float minTol = dyMedian < 0 ? 1+tolerance : 1-tolerance;
    float maxDy1 = dyMedian * maxTol;
    float minDy1 = dyMedian * minTol;
    float maxDy2 = 2*dyMedian * maxTol;
    float minDy2 = 2*dyMedian * minTol;

    Point2f prevPt1;
    Point2f prevPt2;
    int n = 0;
    for (vector<Point2f>::iterator it=pointsXY.begin();
            it!=pointsXY.end(); it++) {
        const Point2f &curPt = *it;
        if (n > 0) {
            LOGDEBUG3("matchGrid() pointsXY[%d] (%g,%g)", n, curPt.x, curPt.y);
            int dy1 = prevPt1.y - curPt.y;
            if (minDy1 <= dy1 && dy1 <= maxDy1) {
                dyTot1 = dyTot1 + (prevPt1 - curPt);
                dyCount1++;
            }
            if (n > 1) {
                int dy2 = prevPt2.y - curPt.y;
                if (minDy2 <= dy2 && dy2 <= maxDy2) {
                    dyTot2 = dyTot2 + (prevPt2 - curPt);
                    dyCount2++;
                }
            }
        }
        prevPt2 = prevPt1;
        prevPt1 = curPt;
        n++;
    }

    string errMsg;
    json_object_set(pStageModel, "dyMedian", json_real(dyMedian));
    json_object_set(pStageModel, "dyCount1", json_integer(dyCount1));
    json_object_set(pStageModel, "dyCount2", json_integer(dyCount2));
    if (dyCount1 == 0) {
        errMsg = "No grid points matched within tolerance (level 1) dyCount1:0";
    } else if (dyCount2 == 0) {
        json_object_set(pStageModel, "dxAvg1", json_real(dyTot1.x/dyCount1));
        json_object_set(pStageModel, "dyAvg1", json_real(dyTot1.y/dyCount1));
        errMsg = "No grid points matched within tolerance (level 2) dyCount2:0";
    } else {
        float dxAvg1 = dyTot1.x/dyCount1;
        float dyAvg1 = dyTot1.y/dyCount1;
        float dxAvg2 = dyTot2.x/dyCount2/2;
        float dyAvg2 = dyTot2.y/dyCount2/2;
        json_object_set(pStageModel, "dydxAvg1", json_real(dxAvg1));
        json_object_set(pStageModel, "dydyAvg1", json_real(dyAvg1));
        json_object_set(pStageModel, "dydxAvg2", json_real(dxAvg2));
        json_object_set(pStageModel, "dydyAvg2", json_real(dyAvg2));
        float normXY = sqrt(dxAvg2*dxAvg2 + dyAvg2*dyAvg2);
        gridY = normXY / sepY;
        json_object_set(pStageModel, "gridY", json_real(gridY));
    }

    return errMsg;
} // identifyRows

static string identifyColumns(json_t *pStageModel, vector<Point2f> &pointsYX, float &dxMedian, Point2f &dxTot1,
                              Point2f &dxTot2, int &dxCount1, int &dxCount2, double tolerance, int sepX, float &gridX)
{
    vector<float> dxList;
    Point2f prevPt;
    for (vector<Point2f>::iterator it=pointsYX.begin(); it!=pointsYX.end(); it++) {
        if (it != pointsYX.begin()) {
            float dx = prevPt.x - it->x;
            dxList.push_back(dx);
        }
        prevPt = *it;
    }
    sort(dxList.begin(), dxList.end());
    dxMedian = dxList[dxList.size()/2];
    float maxTol = dxMedian < 0 ? 1-tolerance : 1+tolerance;
    float minTol = dxMedian < 0 ? 1+tolerance : 1-tolerance;
    float maxDx1 = dxMedian * maxTol;
    float minDx1 = dxMedian * minTol;
    float maxDx2 = 2*dxMedian * maxTol;
    float minDx2 = 2*dxMedian * minTol;

    Point2f prevPt1;
    Point2f prevPt2;
    int n = 0;
    for (vector<Point2f>::iterator it=pointsYX.begin();
            it!=pointsYX.end(); it++) {
        const Point2f &curPt = *it;
        if (n > 0) {
            LOGDEBUG3("matchGrid() pointsYX[%d] (%g,%g)", n, curPt.x, curPt.y);
            int dx1 = prevPt1.x - curPt.x;
            if (minDx1 <= dx1 && dx1 <= maxDx1) {
                dxTot1 = dxTot1 + (prevPt1 - curPt);
                dxCount1++;
            }
            if (n > 1) {
                int dx2 = prevPt2.x - curPt.x;
                if (minDx2 <= dx2 && dx2 <= maxDx2) {
                    dxTot2 = dxTot2 + (prevPt2 - curPt);
                    dxCount2++;
                }
            }
        }
        prevPt2 = prevPt1;
        prevPt1 = curPt;
        n++;
    }

    string errMsg;
    json_object_set(pStageModel, "dxMedian", json_real(dxMedian));
    json_object_set(pStageModel, "dxCount1", json_integer(dxCount1));
    json_object_set(pStageModel, "dxCount2", json_integer(dxCount2));
    if (dxCount1 == 0) {
        errMsg = "No grid points matched within tolerance (level 1) dxCount1:0";
    } else if (dxCount2 == 0) {
        json_object_set(pStageModel, "dxAvg1", json_real(dxTot1.x/dxCount1));
        json_object_set(pStageModel, "dyAvg1", json_real(dxTot1.y/dxCount1));
        errMsg = "No grid points matched within tolerance (level 2) dxCount2:0";
    } else {
        float dxAvg1 = dxTot1.x/dxCount1;
        float dyAvg1 = dxTot1.y/dxCount1;
        float dxAvg2 = dxTot2.x/dxCount2/2;
        float dyAvg2 = dxTot2.y/dxCount2/2;
        json_object_set(pStageModel, "dxdxAvg1", json_real(dxAvg1));
        json_object_set(pStageModel, "dxdyAvg1", json_real(dyAvg1));
        json_object_set(pStageModel, "dxdxAvg2", json_real(dxAvg2));
        json_object_set(pStageModel, "dxdyAvg2", json_real(dyAvg2));
        float normXY = sqrt(dxAvg2*dxAvg2 + dyAvg2*dyAvg2);
        gridX = normXY / sepX;
        json_object_set(pStageModel, "gridX", json_real(gridX));
    }

    return errMsg;
} // identifyColumns

void initializePointMaps(json_t *pRects, vector<Point2f> &pointsXY, vector<Point2f> &pointsYX) {
    json_t *pValue;
    int index;
    json_array_foreach(pRects, index, pValue) {
        json_t *pX = json_object_get(pValue, "x");
        json_t *pY = json_object_get(pValue, "y");
        if (json_is_number(pX) && json_is_number(pY)) {
            double x = json_real_value(pX);
            double y = json_real_value(pY);
            const Point2f key(x,y);
            pointsXY.push_back(key);
            pointsYX.push_back(key);
        }
    }
    const ComparePoint2f cmpXY(COMPARE_XY);
    sort(pointsXY, cmpXY);
    const ComparePoint2f cmpYX(COMPARE_YX);
    sort(pointsYX, cmpYX);
}

inline Point3f calcObjPointDiff(const Point2f &curPt, const Point2f &prevPt, const Point2f &imgSep) {
    float dObjX = (curPt.x-prevPt.x)/imgSep.x;
    float dObjY = (curPt.y-prevPt.y)/imgSep.y;
    dObjX += dObjX < 0 ? -0.5 : 0.5;
    dObjY += dObjY < 0 ? -0.5 : 0.5;
    return Point3f((int)(dObjX), (int) (dObjY), 0);
}

bool Pipeline::apply_matchGrid(json_t *pStage, json_t *pStageModel, Model &model) {
    string rectsModelName = jo_string(pStage, "model", "", model.argMap);
    double objZ = jo_double(pStage, "objZ", 0, model.argMap);
    Point2f objSep(
        jo_double(pStage, "sepX", 5.0, model.argMap),
        jo_double(pStage, "sepY", 5.0, model.argMap));
    double tolerance = jo_double(pStage, "tolerance", 0.35, model.argMap);
    Size imgSize(model.image.cols, model.image.rows);
    Point2f imgCenter(model.image.cols/2.0, model.image.rows/2.0);
    json_t *pRectsModel = json_object_get(model.getJson(false), rectsModelName.c_str());
    string errMsg;

    if (rectsModelName.empty()) {
        errMsg = "matchGrid model: expected name of stage with rects";
    } else if (!json_is_object(pRectsModel)) {
        errMsg = "Named stage is not in model";
    }

    json_t *pRects = NULL;
    if (errMsg.empty()) {
        pRects = json_object_get(pRectsModel, "rects");
        if (!json_is_array(pRects)) {
            errMsg = "Expected array of rects to match";
        } else if (json_array_size(pRects) < 2) {
            errMsg = "Expected array of at least 2 rects to match";
        }
    }

    Point2f dxTot1;
    Point2f dyTot1;
    Point2f dxTot2;
    Point2f dyTot2;
    int dxCount1 = 0;
    int dxCount2 = 0;
    int dyCount1 = 0;
    int dyCount2 = 0;
    float gridX = FLT_MAX;
    float gridY = FLT_MAX;
    Point2f dmedian(FLT_MAX,FLT_MAX);
    const ComparePoint2f cmpYX(COMPARE_YX);
    vector<Point2f> pointsXY;
    vector<Point2f> pointsYX;
	Mat cameraMatrix;
	Mat distCoeffs;

    if (errMsg.empty()) {
        initializePointMaps(pRects, pointsXY, pointsYX);
        errMsg = identifyColumns(pStageModel, pointsYX, dmedian.x,
                                 dxTot1, dxTot2, dxCount1, dxCount2, tolerance, objSep.x, gridX);
        string errMsg2 = identifyRows(pStageModel, pointsXY, dmedian.y,
                                      dyTot1, dyTot2, dyCount1, dyCount2, tolerance, objSep.y, gridY);

        if (errMsg.empty()) {
            errMsg = errMsg2;
        } else if (!errMsg2.empty()) {
            errMsg.append("; ");
            errMsg.append(errMsg2);
        }
    }

    if (errMsg.empty()) {
        Point3f ptObj;
        Point2f ptImg;

        float maxDx1 = dmedian.x * (dmedian.x < 0 ? 1-tolerance : 1+tolerance);
        float minDx1 = dmedian.x * (dmedian.x < 0 ? 1+tolerance : 1-tolerance);
        Point2f imgSep(gridX*objSep.x, gridY*objSep.y);
        cout << "DEBUG minDx1:" << minDx1 << " maxDx1:" << maxDx1 << endl;
        cout << "DEBUG dmedian:" << dmedian << endl;
        cout << "DEBUG imgSep:" << imgSep << endl;
        GridMatcher gm(imgSize, imgSep, objSep);

        vector<Point2f>::iterator itYX = pointsYX.begin();
        for (Point2f ptImg0=(*itYX); ++itYX!=pointsYX.end(); ) {
            const Point2f &ptImg1(*itYX);
            int dx1 = ptImg0.x - ptImg1.x;
            if (minDx1 <= dx1 && dx1 <= maxDx1) {
                if (gm.imagePts.size() == 0) {
                    ptImg = ptImg0;
                    ptObj.x = (int)(ptImg.x/imgSep.x + 0.5);
                    ptObj.y = (int)(ptImg.y/imgSep.y + 0.5);
                    cout << "DEBUG O1 " << ptImg << " " << ptImg0 << " => " << ptObj << endl;
                    gm.add(ptImg, ptObj);
                    ptObj += calcObjPointDiff(ptImg1, ptImg, imgSep);
                    cout << "DEBUG O2 " << ptImg << " " << ptImg1 << " => " << ptObj << endl;
                    ptImg = ptImg1;
                    gm.add(ptImg, ptObj);
                } else {
                    if (ptImg != ptImg0) {
                        ptObj += calcObjPointDiff(ptImg0, ptImg, imgSep);
                        ptImg = ptImg0;
                        gm.add(ptImg, ptObj);
                        cout << "DEBUG A " << ptImg << " " << ptImg0 << " => " << ptObj << endl;
                    }
                    ptObj += calcObjPointDiff(ptImg1, ptImg, imgSep);
                    cout << "DEBUG B " << ptImg << " " << ptImg1 << " => " << ptObj << endl;
                    ptImg = ptImg1;
                    gm.add(ptImg, ptObj);
                }
            } else {
                cout << "DEBUG - " << ptImg << " " << ptImg1 << endl;
            }
            ptImg0 = ptImg1;
        }

        float maxDy1 = dmedian.y * (dmedian.y < 0 ? 1-tolerance : 1+tolerance);
        float minDy1 = dmedian.y * (dmedian.y < 0 ? 1+tolerance : 1-tolerance);
        vector<Point2f>::iterator itXY = pointsXY.begin();
        for (Point2f ptImg0=(*itXY); ++itXY!=pointsXY.end(); ) {
            const Point2f &ptImg1(*itXY);
            int dy1 = ptImg0.y - ptImg1.y;
            if (minDy1 <= dy1 && dy1 <= maxDy1) {
                if (gm.imagePts.size() == 0) {
                    ptImg = ptImg0;
                    ptObj.x = (int)(ptImg.x/imgSep.x + 0.5);
                    ptObj.y = (int)(ptImg.y/imgSep.y + 0.5);
                    cout << "DEBUGy O1 ptImg:" << ptImg << " " << ptImg0 << " => " << ptObj << endl;
                    gm.add(ptImg, ptObj);
                    ptObj += calcObjPointDiff(ptImg1, ptImg, imgSep);
                    cout << "DEBUGy O2 ptImg:" << ptImg << " " << ptImg1 << " => " << ptObj << endl;
                    ptImg = ptImg1;
                    gm.add(ptImg, ptObj);
                } else {
                    if (ptImg != ptImg0) {
                        ptObj += calcObjPointDiff(ptImg0, ptImg, imgSep);
                        ptImg = ptImg0;
                        gm.add(ptImg, ptObj);
                        cout << "DEBUGy A ptImg:" << ptImg << " " << ptImg0 << " => " << ptObj << endl;
                    }
                    ptObj += calcObjPointDiff(ptImg1, ptImg, imgSep);
                    cout << "DEBUGy B ptImg:" << ptImg << " " << ptImg1 << " => " << ptObj << endl;
                    ptImg = ptImg1;
                    gm.add(ptImg, ptObj);
                }
            } else {
                cout << "DEBUGy - ptImg:" << ptImg << " " << ptImg1 << endl;
            }
            ptImg0 = ptImg1;
        }

        Point3f objCentroid = gm.getObjectCentroid();
        Point2f imgCentroid = gm.getImageCentroid();
        cout << "DEBUG objCentroid:" << objCentroid << " imgCentroid:" << imgCentroid << " objectPts:" << gm.size() << endl;
        json_t *pRects = json_array();
        json_object_set(pStageModel, "rects", pRects);
        for (int i=0; i < gm.size(); i++) {
            json_t *pRect = json_object();
            json_object_set(pRect, "x", json_real(gm.imagePts[i].x));
            json_object_set(pRect, "y", json_real(gm.imagePts[i].y));
            json_object_set(pRect, "objX", json_real(objSep.x*(gm.objectPts[i].x-objCentroid.x)));
            json_object_set(pRect, "objY", json_real(objSep.y*(gm.objectPts[i].y-objCentroid.y)));
            json_object_set(pRect, "objZ", json_real(objZ));
            json_array_append(pRects, pRect);
        }

        float totx = 0;
        for (int i=0; i < gm.size(); i++) {
            totx += gm.objectPts[i].x;
            cout << i+1 << ". " << gm.objectPts[i] << endl;
        }
        cout << "totx:" << totx << endl;

        errMsg = gm.calibrateImage(pStageModel, cameraMatrix, distCoeffs);
    }
	if (errMsg.empty()) {
        InputArray newCameraMatrix=noArray();
        Mat dst;
        undistort(model.image, dst, cameraMatrix, distCoeffs, newCameraMatrix);
        model.image = dst;
	}

    return stageOK("apply_matchGrid(%s) %s", errMsg.c_str(), pStage, pStageModel);
}
