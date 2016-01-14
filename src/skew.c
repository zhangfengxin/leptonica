#/*====================================================================*
# -  Copyright (C) 2001 Leptonica.  All rights reserved.
# -  This software is distributed in the hope that it will be
# -  useful, but with NO WARRANTY OF ANY KIND.
# -  No author or distributor accepts responsibility to anyone for the
# -  consequences of using this software, or for whether it serves any
# -  particular purpose or works at all, unless he or she says so in
# -  writing.  Everyone is granted permission to copy, modify and
# -  redistribute this source code, for commercial or non-commercial
# -  purposes, with the following restrictions: (1) the origin of this
# -  source code must not be misrepresented; (2) modified versions must
# -  be plainly marked as such; and (3) this notice may not be removed
# -  or altered from any source or modified source distribution.
# *====================================================================*/

/*
 *  skew.c
 *
 *      Simple top-level deskew interfaces
 *          PIX       *pixDeskew()
 *          PIX       *pixFindSkewAndDeskew()
 *
 *      Simple top-level angle-finding interface
 *          l_int32    pixFindSkew()
 *
 *      Basic angle-finding functions with all parameters
 *          l_int32    pixFindSkewSweep()
 *          l_int32    pixFindSkewSweepAndSearch()
 *          l_int32    pixFindSkewSweepAndSearchScore()
 *
 *      Differential square sum function for scoring
 *          l_int32    pixFindDifferentialSquareSum()
 *
 *
 *      ==============================================================    
 *      Page skew detection
 *
 *      Skew is determined by pixel profiles, which are computed
 *      as pixel sums along the raster line for each line in the
 *      image.  By vertically shearing the image by a given angle,
 *      the sums can be computed quickly along the raster lines
 *      rather than along lines at that angle.  The score is
 *      computed from these line sums by taking the square of
 *      the DIFFERENCE between adjacent line sums, summed over
 *      all lines.  The skew angle is then found as the angle
 *      that maximizes the score.  The actual computation for
 *      any sheared image is done in the function
 *      pixFindDifferentialSquareSum().
 *
 *      The search for the angle that maximizes this score is
 *      most efficiently performed by first sweeping coarsely
 *      over angles, using a significantly reduced image (say, 4x
 *      reduction), to find the approximate maximum within a half
 *      degree or so, and then doing an interval-halving binary
 *      search at higher resolution to get the skew angle to
 *      within 1/20 degree or better.
 *
 *      The differential signal is used (rather than just using
 *      that variance of line sums) because it rejects the
 *      background noise due to total number of black pixels,
 *      and has maximum contributions from the baselines and
 *      x-height lines of text when the textlines are aligned
 *      with the raster lines.  It also works well in multicolumn
 *      pages where the textlines do not line up across columns.
 *
 *      The method is fast, accurate to within an angle (in radians)
 *      of approximately the inverse width in pixels of the image,
 *      and will work on a surprisingly small amount of text data
 *      (just a couple of text lines).  Consequently, it can
 *      also be used to find local skew if the skew were to vary
 *      significantly over the page.  Local skew determination
 *      is not very important except for locating lines of
 *      handwritten text that may be mixed with printed text.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "allheaders.h"

    /* Default sweep angle parameters for pixFindSkew() */
static const l_float32  DEFAULT_SWEEP_RANGE = 5.;    /* degrees */
static const l_float32  DEFAULT_SWEEP_DELTA = 1.;    /* degrees */

    /* Default final angle difference parameter for binary
     * search in pixFindSkew().  The expected accuracy is
     * not better than the inverse image width in pixels,
     * say, 1/2000 radians, or about 0.03 degrees. */
static const l_float32  DEFAULT_MINBS_DELTA = 0.01;  /* degrees */

    /* Default scale factors for pixFindSkew() */
static const l_int32  DEFAULT_SWEEP_REDUCTION = 4;  /* sweep part; 4 is good */
static const l_int32  DEFAULT_BS_REDUCTION = 2;  /* binary search part */

    /* Minimum angle for deskewing in pixDeskew() */
static const l_float32  MIN_DESKEW_ANGLE = 0.1;  /* degree */

    /* Minimum allowed confidence (ratio) for deskewing in pixDeskew() */
static const l_float32  MIN_ALLOWED_CONFIDENCE = 3.0;

    /* Minimum allowed maxscore to give nonzero confidence */
static const l_int32  MIN_VALID_MAXSCORE = 10000;

    /* Constant setting threshold for minimum allowed minscore
     * to give nonzero confidence; multiply this constant by
     *  (height * width^2) */
static const l_float32  MINSCORE_THRESHOLD_CONSTANT = 0.000002;


#ifndef  NO_CONSOLE_IO
#define  DEBUG_PRINT_SCORES     0
#define  DEBUG_PRINT_SWEEP      0
#define  DEBUG_PRINT_BINARY     0
#define  DEBUG_THRESHOLD        0
#define  DEBUG_PLOT_SCORES      0
#endif  /* ~NO_CONSOLE_IO */



/*----------------------------------------------------------------*
 *                       Top-level interfaces                     *
 *----------------------------------------------------------------*/
/*!
 *  pixDeskew()
 *
 *      Input:  pixs  (1 bpp)
 *              redsearch  (for binary search: reduction factor = 1, 2 or 4)
 *      Return: deskewed pix, or null on error
 *
 *  Notes:
 *      (1) This is the most simple high level interface, for 1 bpp input.
 *      (2) It first finds the skew angle.  If the angle is large enough,
 *          it returns a deskewed image; otherwise, it returns a clone.
 */
PIX *
pixDeskew(PIX     *pixs,
          l_int32  redsearch)
{
    PROCNAME("pixDeskew");

    if (!pixs)
        return (PIX *)ERROR_PTR("pixs not defined", procName, NULL);
    if (pixGetDepth(pixs) != 1)
        return (PIX *)ERROR_PTR("pixs not 1 bpp", procName, NULL);
    if (redsearch != 1 && redsearch != 2 && redsearch != 4)
        return (PIX *)ERROR_PTR("redsearch not in {1,2,4}", procName, NULL);

    return pixFindSkewAndDeskew(pixs, redsearch, NULL, NULL);
}


/*!
 *  pixFindSkewAndDeskew()
 *
 *      Input:  pixs  (1 bpp)
 *              redsearch  (for binary search: reduction factor = 1, 2 or 4)
 *              &angle   (<optional return> angle required to deskew,
 *                        in degrees)
 *              &conf    (<optional return> conf value is ratio max/min scores)
 *      Return: deskewed pix, or null on error
 *
 *  Notes:
 *      (1) This first finds the skew angle.  If the angle is large enough,
 *          it returns a deskewed image; otherwise, it returns a clone.
 *      (2) Use NULL for &angle and/or &conf if you don't want those values
 *          returned.
 */
PIX *
pixFindSkewAndDeskew(PIX        *pixs,
                     l_int32     redsearch,
                     l_float32  *pangle,
                     l_float32  *pconf)
{
l_int32    ret;
l_float32  angle, conf, deg2rad;
PIX       *pixd;

    PROCNAME("pixFindSkewAndDeskew");

    if (!pixs)
        return (PIX *)ERROR_PTR("pixs not defined", procName, NULL);
    if (pixGetDepth(pixs) != 1)
        return (PIX *)ERROR_PTR("pixs not 1 bpp", procName, NULL);
    if (redsearch != 1 && redsearch != 2 && redsearch != 4)
        return (PIX *)ERROR_PTR("redsearch not in {1,2,4}", procName, NULL);

    deg2rad = 3.1415926535 / 180.;
    ret = pixFindSkewSweepAndSearch(pixs, &angle, &conf,
                       DEFAULT_SWEEP_REDUCTION, redsearch,
                       DEFAULT_SWEEP_RANGE, DEFAULT_SWEEP_DELTA,
                       DEFAULT_MINBS_DELTA);
    if (pangle)
        *pangle = angle;
    if (pconf)
        *pconf = conf;
    if (ret)
        return pixClone(pixs);

    if (L_ABS(angle) < MIN_DESKEW_ANGLE || conf < MIN_ALLOWED_CONFIDENCE)
        return pixClone(pixs);

    if ((pixd = pixRotateShear(pixs, 0, 0, deg2rad * angle, L_BRING_IN_WHITE))
             == NULL)
        return pixClone(pixs);
    else
        return pixd;
}


/*!
 *  pixFindSkew()
 *
 *      Input:  pixs  (1 bpp)
 *              &angle   (<return> angle required to deskew, in degrees)
 *              &conf    (<return> confidence value is ratio max/min scores)
 *      Return: 0 if OK, 1 on error or if angle measurment not valid
 *
 *  Notes:
 *      (1) This is a simple high-level interface, that uses default
 *          values of the parameters for reasonable speed and accuracy.
 *      (2) The angle returned is the negative of the skew angle of
 *          the image.  It is the angle required for deskew.
 *          Clockwise rotations are positive angles.
 */
l_int32
pixFindSkew(PIX        *pixs,
            l_float32  *pangle,
            l_float32  *pconf)
{
    PROCNAME("pixFindSkew");

    if (!pixs)
        return ERROR_INT("pixs not defined", procName, 1);
    if (pixGetDepth(pixs) != 1)
        return ERROR_INT("pixs not 1 bpp", procName, 1);
    if (!pangle)
        return ERROR_INT("&angle not defined", procName, 1);
    if (!pconf)
        return ERROR_INT("&conf not defined", procName, 1);

    return pixFindSkewSweepAndSearch(pixs, pangle, pconf,
                                     DEFAULT_SWEEP_REDUCTION,
                                     DEFAULT_BS_REDUCTION,
                                     DEFAULT_SWEEP_RANGE,
                                     DEFAULT_SWEEP_DELTA,
                                     DEFAULT_MINBS_DELTA);
}


/*----------------------------------------------------------------*
 *         Basic angle-finding functions with all parameters      *
 *----------------------------------------------------------------*/
/*!
 *  pixFindSkewSweep()
 *
 *      Input:  pixs  (1 bpp)
 *              &angle   (<return> angle required to deskew, in degrees)
 *              reduction  (factor = 1, 2, 4 or 8)
 *              sweeprange   (half the full range; assumed about 0; in degrees)
 *              sweepdelta   (angle increment of sweep; in degrees)
 *      Return: 0 if OK, 1 on error or if angle measurment not valid
 *
 *  Notes:
 *      (1) This examines the 'score' for skew angles with equal intervals.
 *      (2) Caller must check the return value for validity of the result.
 */
l_int32
pixFindSkewSweep(PIX        *pixs,
                 l_float32  *pangle,
                 l_int32     reduction,
                 l_float32   sweeprange,
                 l_float32   sweepdelta)
{
l_int32    bzero, i, nangles;
l_float32  deg2rad, rad2deg, theta;
l_float32  sum, maxscore, maxangle;
NUMA      *natheta, *nascore;
PIX       *pix, *pixt;

    PROCNAME("pixFindSkewSweep");

    if (!pixs)
        return ERROR_INT("pixs not defined", procName, 1);
    if (pixGetDepth(pixs) != 1)
        return ERROR_INT("pixs not 1 bpp", procName, 1);
    if (!pangle)
        return ERROR_INT("&angle not defined", procName, 1);
    if (reduction != 1 && reduction != 2 && reduction != 4 && reduction != 8)
        return ERROR_INT("reduction must be in {1,2,4,8}", procName, 1);

    *pangle = 0.0;  /* init */
    deg2rad = 3.1415926535 / 180.;
    rad2deg = 1. / deg2rad;

        /* Generate reduced image, if requested */
    if (reduction == 1)
        pix = pixClone(pixs);
    else if (reduction == 2)
        pix = pixReduceRankBinaryCascade(pixs, 1, 0, 0, 0);
    else if (reduction == 4)
        pix = pixReduceRankBinaryCascade(pixs, 1, 1, 0, 0);
    else /* reduction == 8 */
        pix = pixReduceRankBinaryCascade(pixs, 1, 1, 2, 0);

    pixZero(pix, &bzero);
    if (bzero) {
        pixDestroy(&pix);
        return 1;
    }

    nangles = (l_int32)((2. * sweeprange) / sweepdelta + 1);
    if ((natheta = numaCreate(nangles)) == NULL)
        return ERROR_INT("natheta not made", procName, 1);
    if ((nascore = numaCreate(nangles)) == NULL)
        return ERROR_INT("nascore not made", procName, 1);

    if ((pixt = pixCreateTemplate(pix)) == NULL)
        return ERROR_INT("pixt not made", procName, 1);

    for (i = 0; i < nangles; i++) {
        theta = -sweeprange + i * sweepdelta;   /* degrees */

            /* Shear pix about the UL corner and put the result in pixt */
        pixVShearCorner(pixt, pix, deg2rad * theta, L_BRING_IN_WHITE);

            /* Get score */
        pixFindDifferentialSquareSum(pixt, &sum);

#if  DEBUG_PRINT_SCORES
        L_INFO_FLOAT2("sum(%7.2f) = %7.0f", procName, theta, sum);
#endif  /* DEBUG_PRINT_SCORES */

            /* Save the result in the output arrays */
        numaAddNumber(nascore, sum);
        numaAddNumber(natheta, theta);
    }

        /* Find the location of the maximum (i.e., the skew angle)
         * by fitting the largest data point and its two neighbors
         * to a quadratic, using lagrangian interpolation.  */
    numaFitMax(nascore, &maxscore, natheta, &maxangle);
    *pangle = maxangle;

#if  DEBUG_PRINT_SWEEP
    L_INFO_FLOAT2(" From sweep: angle = %7.3f, score = %7.3f", procName,
                  maxangle, maxscore);
#endif  /* DEBUG_PRINT_SWEEP */

#if  DEBUG_PLOT_SCORES
        /* Plot the result -- the scores versus rotation angle --
         * using gnuplot with GPLOT_LINES (lines connecting data points).
         * The GPLOT data structure is first created, with the
         * appropriate data incorporated from the two input NUMAs,
         * and then the function gplotMakeOutput() uses gnuplot to
         * generate the output plot.  This can be either a .png file
         * or a .ps file, depending on whether you use GPLOT_PNG
         * or GPLOT_PS.  */
    {GPLOT  *gplot;
        gplot = gplotCreate("sweep_output", GPLOT_PNG,
                    "Sweep. Variance of difference of ON pixels vs. angle",
                    "angle (deg)", "score");
        gplotAddPlot(gplot, natheta, nascore, GPLOT_LINES, "plot1");
        gplotAddPlot(gplot, natheta, nascore, GPLOT_POINTS, "plot2");
        gplotMakeOutput(gplot);
        gplotDestroy(&gplot);
    }
#endif  /* DEBUG_PLOT_SCORES */

    numaDestroy(&nascore);
    numaDestroy(&natheta);
    pixDestroy(&pix);
    pixDestroy(&pixt);
    return 0;
}


/*!
 *  pixFindSkewSweepAndSearch()
 *
 *      Input:  pixs  (1 bpp)
 *              &angle   (<return> angle required to deskew; in degrees)
 *              &conf    (<return> confidence given by ratio of max/min score)
 *              redsweep  (sweep reduction factor = 1, 2, 4 or 8)
 *              redsearch  (binary search reduction factor = 1, 2, 4 or 8;
 *                          and must not exceed redsweep)
 *              sweeprange   (half the full range, assumed about 0; in degrees)
 *              sweepdelta   (angle increment of sweep; in degrees)
 *              minbsdelta   (min binary search increment angle; in degrees)
 *      Return: 0 if OK, 1 on error or if angle measurment not valid
 *
 *  Notes:
 *      (1) This finds the skew angle, doing first a sweep through a set
 *          of equal angles, and then doing a binary search until
 *          convergence.
 *      (2) Caller must check the return value for validity of the result.
 *      (3) See also notes in pixFindSkewSweepAndSearchScore()
 */
l_int32
pixFindSkewSweepAndSearch(PIX        *pixs,
                          l_float32  *pangle,
                          l_float32  *pconf,
                          l_int32     redsweep,
                          l_int32     redsearch,
                          l_float32   sweeprange,
                          l_float32   sweepdelta,
                          l_float32   minbsdelta)
{
    return pixFindSkewSweepAndSearchScore(pixs, pangle, pconf, NULL,
                                          redsweep, redsearch, 0.0, sweeprange, 
                                          sweepdelta, minbsdelta);
}


/*!
 *  pixFindSkewSweepAndSearchScore()
 *
 *      Input:  pixs  (1 bpp)
 *              &angle   (<return> angle required to deskew; in degrees)
 *              &conf    (<return> confidence given by ratio of max/min score)
 *              &endscore (<optional return> max score; use NULL if ignored)
 *              redsweep  (sweep reduction factor = 1, 2, 4 or 8)
 *              redsearch  (binary search reduction factor = 1, 2, 4 or 8;
 *                          and must not exceed redsweep)
 *              sweepcenter  (angle about which sweep is performed; in degrees)
 *              sweeprange   (half the full range, taken about sweepcenter;
 *                            in degrees)
 *              sweepdelta   (angle increment of sweep; in degrees)
 *              minbsdelta   (min binary search increment angle; in degrees)
 *      Return: 0 if OK, 1 on error or if angle measurment not valid
 *
 *  Notes:
 *      (1) This finds the skew angle, doing first a sweep through a set
 *          of equal angles, and then doing a binary search until convergence.
 *      (2) There are two built-in constants that determine if the 
 *          returned confidence is nonzero:
 *            - MIN_VALID_MAXSCORE (minimum allowed maxscore)
 *            - MINSCORE_THRESHOLD_CONSTANT (determines minimum allowed
 *                 minscore, by multiplying by (height * width^2)
 *          If either of these conditions is not satisfied, the returned
 *          confidence value will be zero.  The maxscore is optionally
 *          returned in this function to allow evaluation of the
 *          resulting angle by a method that is independent of the
 *          returned confidence value.
 */
l_int32
pixFindSkewSweepAndSearchScore(PIX        *pixs,
                               l_float32  *pangle,
                               l_float32  *pconf,
                               l_float32  *endscore,
                               l_int32     redsweep,
                               l_int32     redsearch,
                               l_float32   sweepcenter,
                               l_float32   sweeprange,
                               l_float32   sweepdelta,
                               l_float32   minbsdelta)
{
l_int32    bzero, i, nangles, n, ratio, maxindex, minloc;
l_int32    width, height;
l_float32  deg2rad, rad2deg, theta, delta;
l_float32  sum, maxscore, maxangle;
l_float32  centerangle, leftcenterangle, rightcenterangle;
l_float32  lefttemp, righttemp;
l_float32  bsearchscore[5];
l_float32  minscore, minthresh;
l_float32  rangeleft;
NUMA      *natheta, *nascore;
PIX       *pixsw, *pixsch, *pixt1, *pixt2;

    PROCNAME("pixFindSkewSweepAndSearchScore");

    if (!pixs)
        return ERROR_INT("pixs not defined", procName, 1);
    if (pixGetDepth(pixs) != 1)
        return ERROR_INT("pixs not 1 bpp", procName, 1);
    if (!pangle)
        return ERROR_INT("&angle not defined", procName, 1);
    if (!pconf)
        return ERROR_INT("&conf not defined", procName, 1);
    if (redsweep != 1 && redsweep != 2 && redsweep != 4 && redsweep != 8)
        return ERROR_INT("redsweep must be in {1,2,4,8}", procName, 1);
    if (redsearch != 1 && redsearch != 2 && redsearch != 4 && redsearch != 8)
        return ERROR_INT("redsearch must be in {1,2,4,8}", procName, 1);
    if (redsearch > redsweep)
        return ERROR_INT("redsearch must not exceed redsweep", procName, 1);

    *pangle = 0.0;
    *pconf = 0.0;
    deg2rad = 3.1415926535 / 180.;
    rad2deg = 1. / deg2rad;

        /* Generate reduced image for binary search, if requested */
    if (redsearch == 1)
        pixsch = pixClone(pixs);
    else if (redsearch == 2)
        pixsch = pixReduceRankBinaryCascade(pixs, 1, 0, 0, 0);
    else if (redsearch == 4)
        pixsch = pixReduceRankBinaryCascade(pixs, 1, 1, 0, 0);
    else  /* redsearch == 8 */
        pixsch = pixReduceRankBinaryCascade(pixs, 1, 1, 2, 0);

    pixZero(pixsch, &bzero);
    if (bzero) {
        pixDestroy(&pixsch);
        return 1;
    }

        /* Generate reduced image for sweep, if requested */
    ratio = redsweep / redsearch;
    if (ratio == 1)
        pixsw = pixClone(pixsch);
    else {  /* ratio > 1 */
        if (ratio == 2)
            pixsw = pixReduceRankBinaryCascade(pixsch, 1, 0, 0, 0);
        else if (ratio == 4)
            pixsw = pixReduceRankBinaryCascade(pixsch, 1, 2, 0, 0);
        else  /* ratio == 8 */
            pixsw = pixReduceRankBinaryCascade(pixsch, 1, 2, 2, 0);
    }

    if ((pixt1 = pixCreateTemplate(pixsw)) == NULL)
        return ERROR_INT("pixt1 not made", procName, 1);
    if (ratio == 1)
        pixt2 = pixClone(pixt1);
    else {
        if ((pixt2 = pixCreateTemplate(pixsch)) == NULL)
            return ERROR_INT("pixt2 not made", procName, 1);
    }

    nangles = (l_int32)((2. * sweeprange) / sweepdelta + 1);
    if ((natheta = numaCreate(nangles)) == NULL)
        return ERROR_INT("natheta not made", procName, 1);
    if ((nascore = numaCreate(nangles)) == NULL)
        return ERROR_INT("nascore not made", procName, 1);

        /* Do sweep */
    rangeleft = sweepcenter - sweeprange;
    for (i = 0; i < nangles; i++) {
        theta = rangeleft + i * sweepdelta;   /* degrees */

            /* Shear pix about the UL corner and put the result in pixt1 */
        pixVShearCorner(pixt1, pixsw, deg2rad * theta, L_BRING_IN_WHITE);

            /* Get score */
        pixFindDifferentialSquareSum(pixt1, &sum);

#if  DEBUG_PRINT_SCORES
        L_INFO_FLOAT2("sum(%7.2f) = %7.0f", procName, theta, sum);
#endif  /* DEBUG_PRINT_SCORES */

            /* Save the result in the output arrays */
        numaAddNumber(nascore, sum);
        numaAddNumber(natheta, theta);
    }

        /* Find the largest of the set (maxscore at maxangle) */
    numaGetMax(nascore, &maxscore, &maxindex);
    numaGetFValue(natheta, maxindex, &maxangle);

#if  DEBUG_PRINT_SWEEP
    L_INFO_FLOAT2(" From sweep: angle = %7.3f, score = %7.3f", procName,
                  maxangle, maxscore);
#endif  /* DEBUG_PRINT_SWEEP */

#if  DEBUG_PLOT_SCORES
        /* Plot the sweep result -- the scores versus rotation angle --
         * using gnuplot with GPLOT_LINES (lines connecting data points). */
    {GPLOT  *gplot;
        gplot = gplotCreate("sweep_output", GPLOT_PNG,
                    "Sweep. Variance of difference of ON pixels vs. angle",
                    "angle (deg)", "score");
        gplotAddPlot(gplot, natheta, nascore, GPLOT_LINES, "plot1");
        gplotAddPlot(gplot, natheta, nascore, GPLOT_POINTS, "plot2");
        gplotMakeOutput(gplot);
        gplotDestroy(&gplot);
    }
#endif  /* DEBUG_PLOT_SCORES */

        /* Check if the max is at the end of the sweep. */
    n = numaGetCount(natheta);
    if (maxindex == 0 || maxindex == n - 1) {
        L_WARNING("max found at sweep edge", procName);
        goto cleanup;
    }

        /* Empty the numas for re-use */
    numaEmpty(nascore);
    numaEmpty(natheta);

        /* Do binary search to find skew angle.
         * First, set up initial three points. */
    centerangle = maxangle;
    pixVShearCorner(pixt2, pixsch, deg2rad * centerangle, L_BRING_IN_WHITE);
    pixFindDifferentialSquareSum(pixt2, &bsearchscore[2]);
    pixVShearCorner(pixt2, pixsch, deg2rad * (centerangle - sweepdelta),
                    L_BRING_IN_WHITE);
    pixFindDifferentialSquareSum(pixt2, &bsearchscore[0]);
    pixVShearCorner(pixt2, pixsch, deg2rad * (centerangle + sweepdelta),
                    L_BRING_IN_WHITE);
    pixFindDifferentialSquareSum(pixt2, &bsearchscore[4]);

    numaAddNumber(nascore, bsearchscore[2]);
    numaAddNumber(natheta, centerangle);
    numaAddNumber(nascore, bsearchscore[0]);
    numaAddNumber(natheta, centerangle - sweepdelta);
    numaAddNumber(nascore, bsearchscore[4]);
    numaAddNumber(natheta, centerangle + sweepdelta);

        /* Start the search */
    delta = 0.5 * sweepdelta;
    while (delta >= minbsdelta)
    {
            /* Get the left intermediate score */
        leftcenterangle = centerangle - delta;
        pixVShearCorner(pixt2, pixsch, deg2rad * leftcenterangle,
                        L_BRING_IN_WHITE);
        pixFindDifferentialSquareSum(pixt2, &bsearchscore[1]);
        numaAddNumber(nascore, bsearchscore[1]);
        numaAddNumber(natheta, leftcenterangle);
        
            /* Get the right intermediate score */
        rightcenterangle = centerangle + delta;
        pixVShearCorner(pixt2, pixsch, deg2rad * rightcenterangle,
                        L_BRING_IN_WHITE);
        pixFindDifferentialSquareSum(pixt2, &bsearchscore[3]);
        numaAddNumber(nascore, bsearchscore[3]);
        numaAddNumber(natheta, rightcenterangle);
        
            /* Find the maximum of the five scores and its location.
             * Note that the maximum must be in the center
             * three values, not in the end two. */
        maxscore = bsearchscore[1];
        maxindex = 1;
        for (i = 2; i < 4; i++) {
            if (bsearchscore[i] > maxscore) {
                maxscore = bsearchscore[i];
                maxindex = i;
            }
        }

            /* Set up score array to interpolate for the next iteration */
        lefttemp = bsearchscore[maxindex - 1];
        righttemp = bsearchscore[maxindex + 1];
        bsearchscore[2] = maxscore;
        bsearchscore[0] = lefttemp;
        bsearchscore[4] = righttemp;

            /* Get new center angle and delta for next iteration */
        centerangle = centerangle + delta * (maxindex - 2);
        delta = 0.5 * delta;
    }
    *pangle = centerangle;

#if  DEBUG_PRINT_SCORES
    L_INFO_FLOAT(" Binary search score = %7.3f", procName, bsearchscore[2]);
#endif  /* DEBUG_PRINT_SCORES */
    if(endscore) {
      *endscore = bsearchscore[2];
    }

    if (endscore)  /* save if requested */
        *endscore = bsearchscore[2];

        /* Return the ratio of Max score over Min score
         * as a confidence value.  Don't trust if the Min score
         * is too small, which can happen if the image is all black
         * with only a few white pixels interspersed.  In that case,
         * we get a contribution from the top and bottom edges when
         * vertically sheared, but this contribution becomes zero when
         * the shear angle is zero.  For zero shear angle, the only
         * contribution will be from the white pixels.  We expect that
         * the signal goes as the product of the (height * width^2),
         * so we compute a (hopefully) normalized minimum threshold as
         * a function of these dimensions.  */
    numaGetMin(nascore, &minscore, &minloc);
    width = pixGetWidth(pixsch);
    height = pixGetHeight(pixsch);
    minthresh = MINSCORE_THRESHOLD_CONSTANT * width * width * height;

#if  DEBUG_THRESHOLD
    L_INFO_FLOAT2(" minthresh = %10.2f, minscore = %10.2f", procName,
            minthresh, minscore);
#endif  /* DEBUG_THRESHOLD */

    if (minscore > minthresh)
        *pconf = maxscore / minscore;
    else
        *pconf = 0.0;

        /* Don't trust it if too close to the edge of the sweep
         * range or if maxscore is small */
    if ((centerangle > rangeleft + 2 * sweeprange - sweepdelta) ||
        (centerangle < rangeleft + sweepdelta) ||
        (maxscore < MIN_VALID_MAXSCORE))
        *pconf = 0.0;

#if  DEBUG_PRINT_BINARY
    L_INFO_FLOAT2(" From binary search: angle = %7.3f, score ratio = %8.2f",
                  procName, *pangle, *pconf);
#endif  /* DEBUG_PRINT_BINARY */

#if  DEBUG_PLOT_SCORES
        /* Plot the result -- the scores versus rotation angle --
         * using gnuplot with GPLOT_POINTS.  Because the data
         * points are not ordered by theta (increasing or decreasing),
         * using GPLOT_LINES would be confusing! */
    {GPLOT  *gplot;
        gplot = gplotCreate("search_output", GPLOT_PNG,
                "Binary search.  Variance of difference of ON pixels vs. angle",
                "angle (deg)", "score");
        gplotAddPlot(gplot, natheta, nascore, GPLOT_POINTS, "plot1");
        gplotMakeOutput(gplot);
        gplotDestroy(&gplot);
    }
#endif  /* DEBUG_PLOT_SCORES */

cleanup:
    numaDestroy(&nascore);
    numaDestroy(&natheta);
    pixDestroy(&pixsw);
    pixDestroy(&pixsch);
    pixDestroy(&pixt1);
    pixDestroy(&pixt2);

    return 0;
}



/*----------------------------------------------------------------*
 *                  Differential square sum function              *
 *----------------------------------------------------------------*/
/*!
 *  pixFindDifferentialSquareSum()
 *
 *      Input:  pixs
 *              &sum  (<return> result)
 *      Return: 0 if OK, 1 on error
 */
l_int32
pixFindDifferentialSquareSum(PIX        *pixs,
                             l_float32  *psum)
{
l_int32    i, n;
l_int32    w, h, skiph, skip, nskip;
l_float32  val1, val2, diff, sum;
NUMA      *na;

    PROCNAME("pixFindDifferentialSquareSum");

    if (!pixs)
        return ERROR_INT("pixs not defined", procName, 1);

        /* Generate a number array consisting of the sum
         * of pixels in each row of pixs */
    if ((na = pixCountPixelsByRow(pixs, NULL)) == NULL)
        return ERROR_INT("na not made", procName, 1);

        /* Compute the number of rows at top and bottom to omit.
         * We omit these to avoid getting a spurious signal from
         * the top and bottom of a (nearly) all black image. */
    w = pixGetWidth(pixs);
    h = pixGetHeight(pixs);
    skiph = (l_int32)(0.05 * w);  /* skip for max shear of 0.025 radians */
    skip = L_MIN(h / 10, skiph);  /* don't remove more than 10% of image */
    nskip = L_MAX(skip / 2, 1);  /* at top & bot; skip at least one line */

        /* Sum the squares of differential row sums, on the
         * allowed rows.  Note that nskip must be >= 1. */
    n = numaGetCount(na);
    sum = 0.0;
    for (i = nskip; i < n - nskip; i++) {
        numaGetFValue(na, i - 1, &val1);
        numaGetFValue(na, i, &val2);
        diff = val2 - val1;
        sum += diff * diff;
    }
    numaDestroy(&na);
    *psum = sum;
    return 0;
}
