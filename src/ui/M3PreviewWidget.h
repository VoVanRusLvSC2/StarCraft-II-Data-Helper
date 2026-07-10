#pragma once

#include <QByteArray>
#include <QPoint>
#include <QVector3D>
#include <QWidget>

class M3PreviewWidget final : public QWidget
{
public:
    explicit M3PreviewWidget(QWidget *parent = nullptr);
    bool setModelData(const QByteArray &data, QString *error = nullptr);
    void clearModel();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct Face { quint32 a = 0, b = 0, c = 0; };
    QVector<QVector3D> m_vertices;
    QVector<Face> m_faces;
    QPoint m_lastMouse;
    float m_yaw = -35.0f;
    float m_pitch = 20.0f;
    float m_zoom = 1.0f;
};
