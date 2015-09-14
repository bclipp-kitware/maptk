/*ckwg +29
 * Copyright 2015 by Kitware, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither name of Kitware, Inc. nor the names of any contributors may be used
 *    to endorse or promote products derived from this software without specific
 *    prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "MainWindow.h"

#include "Project.h"

#include <maptk/camera_io.h>
#include <maptk/landmark_map_io.h>

#include <vtkAppendPolyData.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkFrustumSource.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkPlanes.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindow.h>

#include <qtUiState.h>

#include <QtGui/QApplication>
#include <QtGui/QFileDialog>

#include <QtCore/QDebug>
#include <QtCore/QTimer>

namespace // anonymous
{

//-----------------------------------------------------------------------------
struct Camera {
  maptk::vector_3d center; // Camera position
  maptk::vector_3d view; // Direction vector of camera view
  maptk::vector_3d up; // Direction vector of camera up axis
  double fov; // Camera field-of-view angle, in degrees
  double aspect; // Camera aspect ratio (image width / image height)
};

//-----------------------------------------------------------------------------
struct CameraData
{
  Camera camera; // Description of camera
  QString imagePath; // Full path to camera image data
  QSize imageDimensions; // Dimensions of image data
};

//-----------------------------------------------------------------------------
double computeFov(double width, double length)
{
  return vtkMath::DegreesFromRadians(2.0 * atan(0.5 * width / length));
}

//-----------------------------------------------------------------------------
Camera buildCamera(maptk::camera_d const& camera, QSizeF const& imageSize)
{
  Camera out;

  // Get camera parameters
  auto const& ci = camera.get_intrinsics();
  auto const pixelAspect = ci.aspect_ratio();
  auto const focalLength = ci.focal_length();

  out.aspect = pixelAspect * imageSize.width() / imageSize.height();
  out.fov = computeFov(imageSize.height(), focalLength);

  // Compute camera vectors from matrix
  auto const& rotationMatrix =
    camera.get_rotation().quaternion().toRotationMatrix();

  out.up = -rotationMatrix.row(1).transpose();
  out.view = rotationMatrix.row(2).transpose();
  out.center = camera.get_center();

  return out;
}

//-----------------------------------------------------------------------------
void buildFrustum(vtkPlanes* out, Camera const& c)
{
  auto const depth = 15.0; // TODO make configurable or something
  auto const& focus = c.center + (c.view * depth / c.view.norm());

  vtkNew<vtkCamera> camera;

  camera->SetPosition(c.center[0], c.center[1], c.center[2]);
  camera->SetFocalPoint(focus[0], focus[1], focus[2]);
  camera->SetViewUp(c.up[0], c.up[1], c.up[2]);
  camera->SetViewAngle(c.fov);
  camera->SetClippingRange(0.01, depth);

  double planeCoeffs[24];
  camera->GetFrustumPlanes(c.aspect, planeCoeffs);
  out->SetFrustumPlanes(planeCoeffs);
}

} // namespace <anonymous>

//-----------------------------------------------------------------------------
class MainWindowPrivate
{
public:
  void addCamera(CameraData const&);

  Ui::MainWindow UI;
  qtUiState uiState;

  QTimer slideTimer;

  vtkNew<vtkRenderer> renderer;
  vtkNew<vtkRenderWindow> renderWindow;

  vtkNew<vtkAppendPolyData> cameraData;
  QList<CameraData> cameras;
};

QTE_IMPLEMENT_D_FUNC(MainWindow)

//-----------------------------------------------------------------------------
void MainWindowPrivate::addCamera(CameraData const& cd)
{
  // Build frustum from camera data
  vtkNew<vtkPlanes> planes;
  buildFrustum(planes.GetPointer(), cd.camera);

  vtkNew<vtkFrustumSource> frustum;
  frustum->SetPlanes(planes.GetPointer());
  frustum->SetShowLines(false);
  frustum->Update();

  // Make a copy of the frustum mesh so we can modify it
  vtkNew<vtkPolyData> polyData;
  polyData->DeepCopy(frustum->GetOutput());
  auto frustumPoints = polyData->GetPoints();

  // Add a polygon to indicate the up direction (the far plane uses points
  // 0, 1, 2, and 3, with 2 and 3 on the top; we use those with the center of
  // the far polygon to compute a point "above" the far face to form a triangle
  // like the roof of a "house")
  //
  // TODO vtkFrustumSource indicates that this is actually the near plane, but
  //      appears to be wrong - need to verify
  maptk::vector_3d points[4];
  frustumPoints->GetPoint(0, points[0].data());
  frustumPoints->GetPoint(1, points[1].data());
  frustumPoints->GetPoint(2, points[2].data());
  frustumPoints->GetPoint(3, points[3].data());

  // Compute new point:
  //   center = (p0 + p1 + p2 + p3) / 4.0
  //   top = (p2 + p3) / 2.0
  //   new = top + (top - center)
  //       = p2 + p3 - center
  auto const center = 0.25 * (points[0] + points[1] + points[2] + points[3]);
  auto const newPoint = maptk::vector_3d(points[2] + points[3] - center);

  // Insert new point and new face
  vtkIdType newIndex = frustumPoints->InsertNextPoint(newPoint.data());
  vtkCellArray* polys = polyData->GetPolys();
  vtkIdType pts[3] = { 2, 3, newIndex };
  polys->InsertNextCell(3, pts);

  // Add mesh to camera meshes
  this->cameraData->AddInputData(polyData.GetPointer());

  // Add camera data to camera list
  this->cameras.append(cd);

  this->UI.playSlideshow->setEnabled(true);
  this->UI.camera->setEnabled(true);
  this->UI.camera->setRange(0, this->cameras.count() - 1);
}

//-----------------------------------------------------------------------------
MainWindow::MainWindow() : d_ptr(new MainWindowPrivate)
{
  QTE_D();

  // Set up UI
  d->UI.setupUi(this);

  d->uiState.mapState("Window/state", this);
  d->uiState.mapGeometry("Window/geometry", this);
  d->uiState.restore();

  connect(d->UI.actionOpen, SIGNAL(triggered()), this, SLOT(openFile()));
  connect(d->UI.actionQuit, SIGNAL(triggered()), qApp, SLOT(quit()));

  connect(&d->slideTimer, SIGNAL(timeout()), this, SLOT(nextSlide()));
  connect(d->UI.playSlideshow, SIGNAL(toggled(bool)),
          this, SLOT(setSlideshowPlaying(bool)));
  connect(d->UI.slideDelay, SIGNAL(valueChanged(int)),
          this, SLOT(setSlideDelay(int)));

  this->setSlideDelay(d->UI.slideDelay->value());

  // Set up render pipeline
  d->renderer->SetBackground(0, 0, 0);
  d->renderWindow->AddRenderer(d->renderer.GetPointer());
  d->UI.renderWidget->SetRenderWindow(d->renderWindow.GetPointer());

  // Set up actor for camera frustums
  vtkNew<vtkActor> cameraActor;
  vtkNew<vtkPolyDataMapper> cameraMapper;
  cameraMapper->SetInputConnection(d->cameraData->GetOutputPort());
  cameraActor->SetMapper(cameraMapper.GetPointer());
  cameraActor->GetProperty()->SetRepresentationToWireframe();

  d->renderer->AddActor(cameraActor.GetPointer());
}

//-----------------------------------------------------------------------------
MainWindow::~MainWindow()
{
  QTE_D();
  d->uiState.save();
}

//-----------------------------------------------------------------------------
void MainWindow::openFile()
{
  auto const paths = QFileDialog::getOpenFileNames(
    this, "Open File", QString(),
    "All Supported Files (*.conf *.ply *.krtd);;"
    "Project configuration file (*.conf);;"
    "Landmark file (*.ply);;"
    "Camera file (*.krtd);;"
    "All Files (*)");

  if (!paths.isEmpty())
  {
    this->openFiles(paths);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::openFile(QString const& path)
{
  auto const fi = QFileInfo(path);
  if (fi.suffix().toLower() == "conf")
  {
    this->loadProject(path);
  }
  else if (fi.suffix().toLower() == "ply")
  {
    this->loadLandmarks(path);
  }
  else if (fi.suffix().toLower() == "krtd")
  {
    this->loadCamera(path);
  }
  else
  {
    qWarning() << "Don't know how to read file" << path
               << "(unrecognized extension)";
  }
}

//-----------------------------------------------------------------------------
void MainWindow::openFiles(QStringList const& paths)
{
  foreach (auto const& path, paths)
  {
    this->openFile(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::loadProject(const QString& path)
{
  QTE_D();

  Project project;
  if (!project.read(path))
  {
    qWarning() << "Failed to load project from" << path; // TODO dialog?
    return;
  }

  this->loadLandmarks(project.landmarks);

  auto const cameraDir = maptk::path_t(qPrintable(project.cameraPath));
  foreach (auto const& ip, project.images)
  {
    auto const& camera = maptk::read_krtd_file(qPrintable(ip), cameraDir);

    // Build camera data
    CameraData cd;
    cd.imagePath = ip;
    cd.imageDimensions = QImage(ip).size();
    cd.camera = buildCamera(camera, cd.imageDimensions);

    // Add camera to scene
    d->addCamera(cd);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::loadCamera(const QString& path)
{
  QTE_D();

  auto const& camera = maptk::read_krtd_file(qPrintable(path));

  // Guess image size
  auto const& s = camera.get_intrinsics().principal_point() * 2.0;
  auto const imageSize = QSizeF(s[0], s[1]);

  // Build camera data
  CameraData cd;
  cd.camera = buildCamera(camera, imageSize);
  cd.imageDimensions = imageSize.toSize();

  // Add camera to scene
  d->addCamera(cd);
}

//-----------------------------------------------------------------------------
void MainWindow::loadLandmarks(const QString& path)
{
  QTE_D();

  auto const& landmarksPtr = maptk::read_ply_file(qPrintable(path));
  auto const& landmarks = landmarksPtr->landmarks();

  vtkNew<vtkPoints> points;
  vtkNew<vtkCellArray> verts;

  points->Allocate(static_cast<vtkIdType>(landmarks.size()));
  verts->Allocate(static_cast<vtkIdType>(landmarks.size()));

  vtkIdType vertIndex = 0;
  for (auto i = landmarks.cbegin(); i != landmarks.cend(); ++i)
  {
    auto const id = i->first;
    auto const& pos = i->second->loc();
    points->InsertNextPoint(pos.data());
    verts->InsertNextCell(1);
    verts->InsertCellPoint(vertIndex++);
  }

  vtkNew<vtkPolyData> polyData;
  vtkNew<vtkPolyDataMapper> mapper;

  polyData->SetPoints(points.GetPointer());
  polyData->SetVerts(verts.GetPointer());
  mapper->SetInputData(polyData.GetPointer());

  vtkNew<vtkActor> actor;
  actor->SetMapper(mapper.GetPointer());
  actor->GetProperty()->SetPointSize(2);
  d->renderer->AddActor(actor.GetPointer());
}

//-----------------------------------------------------------------------------
void MainWindow::setSlideDelay(int delayExp)
{
  QTE_D();

  auto const de = static_cast<double>(delayExp) * 0.1;
  d->slideTimer.setInterval(qRound(pow(10.0, de)));
}

//-----------------------------------------------------------------------------
void MainWindow::setSlideshowPlaying(bool playing)
{
  QTE_D();
  if (playing)
  {
    if (d->UI.camera->value() == d->UI.camera->maximum())
    {
      d->UI.camera->triggerAction(QAbstractSlider::SliderToMinimum);
    }
    d->slideTimer.start();
  }
  else
  {
    d->slideTimer.stop();
  }

  d->UI.camera->setEnabled(!playing);
}

//-----------------------------------------------------------------------------
void MainWindow::nextSlide()
{
  QTE_D();

  if (d->UI.camera->value() == d->UI.camera->maximum())
  {
    if (d->UI.loopSlideshow->isChecked())
    {
      d->UI.camera->triggerAction(QAbstractSlider::SliderToMinimum);
    }
    else
    {
      d->UI.playSlideshow->setChecked(false);
    }
  }
  else
  {
    d->UI.camera->triggerAction(QAbstractSlider::SliderSingleStepAdd);
  }
}
