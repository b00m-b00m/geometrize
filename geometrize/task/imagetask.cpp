#include "imagetask.h"

#include <atomic>
#include <cassert>
#include <functional>
#include <memory>
#include <vector>

#include <QThread>

#include "geometrize/commonutil.h"
#include "geometrize/bitmap/bitmap.h"
#include "geometrize/rasterizer/rasterizer.h"
#include "geometrize/shape/shapemutator.h"
#include "geometrize/runner/imagerunner.h"
#include "geometrize/model.h"
#include "geometrize/shaperesult.h"
#include "geometrize/shape/shape.h"
#include "geometrize/shape/rectangle.h"
#include "geometrize/shape/shapefactory.h"

#include "preferences/imagetaskpreferences.h"
#include "script/geometrizerengine.h"
#include "task/imagetaskworker.h"

namespace geometrize
{

namespace task
{

class ImageTask::ImageTaskImpl
{
public:
    ImageTaskImpl(ImageTask* pQ, const std::string& displayName, Bitmap& bitmap, Qt::ConnectionType workerConnectionType) :
        q{pQ}, m_preferences{}, m_displayName{displayName}, m_id{getId()}, m_worker{bitmap}
    {
        init(workerConnectionType);
    }

    ImageTaskImpl(ImageTask* pQ, const std::string& displayName, Bitmap& bitmap, const Bitmap& initial, Qt::ConnectionType workerConnectionType) :
        q{pQ}, m_preferences{}, m_displayName{displayName}, m_id{getId()}, m_worker{bitmap, initial}
    {
        init(workerConnectionType);
    }

    ~ImageTaskImpl()
    {
        disconnectAll();

        m_workerThread.quit();
        if(!m_workerThread.wait(1000)) {
            m_workerThread.terminate();
            m_workerThread.wait();
        }
    }

    ImageTaskImpl& operator=(const ImageTaskImpl&) = delete;
    ImageTaskImpl(const ImageTaskImpl&) = delete;

    geometrize::script::GeometrizerEngine& getGeometrizer()
    {
        return m_geometrizer;
    }

    Bitmap& getTarget()
    {
        return m_worker.getTarget();
    }

    Bitmap& getCurrent()
    {
        return m_worker.getCurrent();
    }

    const Bitmap& getTarget() const
    {
        return m_worker.getTarget();
    }

    const Bitmap& getCurrent() const
    {
        return m_worker.getCurrent();
    }

    std::uint32_t getWidth() const
    {
        return m_worker.getCurrent().getWidth();
    }

    std::uint32_t getHeight() const
    {
        return m_worker.getCurrent().getHeight();
    }

    std::string getDisplayName() const
    {
        return m_displayName;
    }

    std::size_t getTaskId() const
    {
        return m_id;
    }

    bool isStepping() const
    {
        return m_worker.isStepping();
    }

    void stepModel()
    {
        const auto shapeCreator = [this]() {
            const std::int32_t w = m_worker.getTarget().getWidth();
            const std::int32_t h = m_worker.getTarget().getHeight();

            if(m_preferences.isScriptModeEnabled()) {
                return m_geometrizer.makeShapeCreator(m_preferences.getImageRunnerOptions().shapeTypes, w, h);
            }
            return geometrize::createDefaultShapeCreator(m_preferences.getImageRunnerOptions().shapeTypes, w, h);
        }();
        emit q->signal_step(m_preferences.getImageRunnerOptions(), shapeCreator);
    }

    void drawShape(const std::shared_ptr<geometrize::Shape> shape, const geometrize::rgba color)
    {
        emit q->signal_drawShape(shape, color);
    }

    void drawBackgroundRectangle()
    {
        const geometrize::rgba color{geometrize::commonutil::getAverageImageColor(m_worker.getRunner().getTarget())};
        const std::int32_t w = m_worker.getTarget().getWidth();
        const std::int32_t h = m_worker.getTarget().getHeight();

        const std::shared_ptr<geometrize::Rectangle> rectangle = std::make_shared<geometrize::Rectangle>(0, 0, w, h);
        rectangle->rasterize = [w, h](const geometrize::Shape& s) { return geometrize::rasterize(static_cast<const geometrize::Rectangle&>(s), w, h); };

        emit q->signal_drawShape(rectangle, color);
    }

    void modelWillStep()
    {
        emit q->signal_modelWillStep();
    }

    void modelDidStep(const std::vector<geometrize::ShapeResult> shapes)
    {
        emit q->signal_modelDidStep(shapes);
    }

    preferences::ImageTaskPreferences& getPreferences()
    {
        return m_preferences;
    }

    void setPreferences(const preferences::ImageTaskPreferences preferences)
    {
        m_preferences = preferences;
        emit q->signal_preferencesSet();
    }

private:
    static std::size_t getId()
    {
        static std::atomic<std::size_t> id{0U};
        return id++;
    }

    void init(const Qt::ConnectionType connectionType)
    {
        qRegisterMetaType<std::vector<geometrize::ShapeResult>>();
        qRegisterMetaType<geometrize::ImageRunnerOptions>();
        qRegisterMetaType<std::function<std::shared_ptr<geometrize::Shape>()>>();
        qRegisterMetaType<std::shared_ptr<geometrize::Shape>>();
        qRegisterMetaType<geometrize::rgba>();

        m_worker.moveToThread(&m_workerThread);
        m_workerThread.start();

        connectSignals(connectionType);
    }

    void connectSignals(const Qt::ConnectionType connectionType)
    {
        q->connect(q, &ImageTask::signal_step, &m_worker, &ImageTaskWorker::step, connectionType);
        q->connect(q, &ImageTask::signal_drawShape, &m_worker, &ImageTaskWorker::drawShape, connectionType);
        q->connect(&m_worker, &ImageTaskWorker::signal_willStep, q, &ImageTask::modelWillStep, Qt::BlockingQueuedConnection);
        q->connect(&m_worker, &ImageTaskWorker::signal_didStep, q, &ImageTask::modelDidStep, Qt::BlockingQueuedConnection);
    }

    void disconnectAll()
    {
        q->disconnect(q, &ImageTask::signal_step, &m_worker, &ImageTaskWorker::step);
        q->disconnect(q, &ImageTask::signal_drawShape, &m_worker, &ImageTaskWorker::drawShape);
        q->disconnect(&m_worker, &ImageTaskWorker::signal_willStep, q, &ImageTask::modelWillStep);
        q->disconnect(&m_worker, &ImageTaskWorker::signal_didStep, q, &ImageTask::modelDidStep);
    }

    ImageTask* q;
    preferences::ImageTaskPreferences m_preferences; ///> Runtime configuration parameters for the runner.
    const std::string m_displayName; ///> The display name of the image task.
    const std::size_t m_id; ///> A unique id for the image task.
    QThread m_workerThread; ///> Thread that the image task worker runs on.
    ImageTaskWorker m_worker; ///> The image task worker.
    geometrize::script::GeometrizerEngine m_geometrizer; ///> The script-based geometrizer for the image task.
};

ImageTask::ImageTask(Bitmap& target, Qt::ConnectionType workerConnectionType) : QObject(),
    d{std::make_unique<ImageTask::ImageTaskImpl>(this, "", target, workerConnectionType)}
{
}

ImageTask::ImageTask(Bitmap& target, Bitmap& background, Qt::ConnectionType workerConnectionType) : QObject(),
    d{std::make_unique<ImageTask::ImageTaskImpl>(this, "", target, background, workerConnectionType)}
{
}

ImageTask::ImageTask(const std::string& displayName, Bitmap& bitmap, Qt::ConnectionType workerConnectionType) : QObject(),
    d{std::make_unique<ImageTask::ImageTaskImpl>(this, displayName, bitmap, workerConnectionType)}
{
}

ImageTask::ImageTask(const std::string& displayName, Bitmap& bitmap, const Bitmap& initial, Qt::ConnectionType workerConnectionType) : QObject(),
    d{std::make_unique<ImageTask::ImageTaskImpl>(this, displayName, bitmap, initial, workerConnectionType)}
{
}

ImageTask::~ImageTask()
{
}

Bitmap& ImageTask::getTarget()
{
    return d->getTarget();
}

Bitmap& ImageTask::getCurrent()
{
    return d->getCurrent();
}

const Bitmap& ImageTask::getTarget() const
{
    return d->getTarget();
}

const Bitmap& ImageTask::getCurrent() const
{
    return d->getCurrent();
}

std::uint32_t ImageTask::getWidth() const
{
    return d->getWidth();
}

std::uint32_t ImageTask::getHeight() const
{
    return d->getHeight();
}

std::string ImageTask::getDisplayName() const
{
    return d->getDisplayName();
}

std::size_t ImageTask::getTaskId() const
{
    return d->getTaskId();
}

bool ImageTask::isStepping() const
{
    return d->isStepping();
}

void ImageTask::stepModel()
{
    d->stepModel();
}

void ImageTask::drawShape(std::shared_ptr<geometrize::Shape> shape, const geometrize::rgba color)
{
    d->drawShape(shape, color);
}

void ImageTask::drawBackgroundRectangle()
{
    d->drawBackgroundRectangle();
}

void ImageTask::modelWillStep()
{
    d->modelWillStep();
}

void ImageTask::modelDidStep(std::vector<geometrize::ShapeResult> shapes)
{
    d->modelDidStep(shapes);
}

preferences::ImageTaskPreferences& ImageTask::getPreferences()
{
    return d->getPreferences();
}

void ImageTask::setPreferences(const preferences::ImageTaskPreferences preferences)
{
    d->setPreferences(preferences);
}

geometrize::script::GeometrizerEngine& ImageTask::getGeometrizer()
{
    return d->getGeometrizer();
}

}

}
