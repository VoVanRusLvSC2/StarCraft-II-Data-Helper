#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <array>

// Renderer-independent representation of the supported static M3 content.
// Animation data is intentionally separate so unsupported sequence versions
// never invalidate a model's geometry/material preview.
struct M3Vertex
{
    QVector3D position;
    QVector3D normal;
    QVector2D uv;
    std::array<quint8, 4> boneWeights = {};
    std::array<quint8, 4> boneIndices = {};
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

struct M3Bone
{
    QString name;
    qint16 parent = -1;
    QVector3D restPosition;
    QVector4D restRotation;
    QVector3D restScale = QVector3D(1.0f, 1.0f, 1.0f);
};

struct M3Model
{
    QString name;
    QVector<M3Vertex> vertices;
    QVector<M3Triangle> triangles;
    QVector<M3Material> materials;
    QVector<M3AnimationSequence> sequences;
    QVector<M3Bone> bones;
    int boneCount = 0;
    QStringList diagnostics;
};
