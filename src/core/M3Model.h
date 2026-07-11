#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QVector2D>
#include <QVector3D>

// Renderer-independent representation of the supported static M3 content.
// Animation data is intentionally separate so unsupported sequence versions
// never invalidate a model's geometry/material preview.
struct M3Vertex
{
    QVector3D position;
    QVector3D normal;
    QVector2D uv;
};

struct M3Triangle
{
    quint32 a = 0;
    quint32 b = 0;
    quint32 c = 0;
    int materialIndex = -1;
};

struct M3Material
{
    QString name;
    QString diffuseTexturePath;
    QColor fallbackColor = QColor(20, 165, 122);
};

struct M3AnimationSequence
{
    QString name;
    quint32 startFrame = 0;
    quint32 endFrame = 0;
};

struct M3Model
{
    QString name;
    QVector<M3Vertex> vertices;
    QVector<M3Triangle> triangles;
    QVector<M3Material> materials;
    QVector<M3AnimationSequence> sequences;
};
